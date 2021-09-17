/* Copyright (c) Mark Harmstone 2020
 *
 * This file is part of ntfs2btrfs.
 *
 * Ntfs2btrfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public Licence as published by
 * the Free Software Foundation, either version 2 of the Licence, or
 * (at your option) any later version.
 *
 * Ntfs2btrfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public Licence for more details.
 *
 * You should have received a copy of the GNU General Public Licence
 * along with Ntfs2btrfs. If not, see <https://www.gnu.org/licenses/>. */

#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING

#include "ntfs.h"
#include "ntfs2btrfs.h"
#include "crc32c.h"
#include "xxhash.h"

#if defined(__i386__) || defined(__x86_64__)
#ifndef _MSC_VER
#include <cpuid.h>
#else
#include <intrin.h>
#endif
#endif

#include <iostream>
#include <new>
#include <chrono>
#include <random>
#include <locale>
#include <codecvt>
#include <optional>

#ifdef _WIN32
#include <windows.h>
#endif

#include "config.h"

using namespace std;

list<chunk> chunks;
list<root> roots;
uint32_t tree_size = 0x4000; // FIXME
list<space> space_list;
bool chunks_changed;
uint64_t data_size = 0;
BTRFS_UUID fs_uuid, chunk_uuid, dev_uuid, subvol_uuid;
list<relocation> relocs;
uint64_t device_size, orig_device_size;
bool reloc_last_sector = false;
uint64_t mapped_inodes = 0, rewritten_inodes = 0, inline_inodes = 0;

static const uint64_t stripe_length = 0x10000;
static const uint64_t chunk_virt_offset = 0x100000;
static const uint64_t dummy_inode = 0xffffffffffffffff; // protected data

static const uint64_t first_ntfs_inode = 24;

static const uint64_t data_chunk_size = 128 * 1024 * 1024; // FIXME

static const uint64_t inode_offset = 0x101;

static const uint16_t max_inline = 2048;
static const uint64_t max_extent_size = 0x8000000; // 128 MB
static const uint64_t max_comp_extent_size = 0x20000; // 128 KB

#define EA_NTACL "security.NTACL"
#define EA_NTACL_HASH 0x45922146

#define EA_DOSATTRIB "user.DOSATTRIB"
#define EA_DOSATTRIB_HASH 0x914f9939

#define EA_REPARSE "user.reparse"
#define EA_REPARSE_HASH 0xfabad1fe

using runs_t = map<uint64_t, list<data_alloc>>;

static void space_list_remove(list<space>& space_list, uint64_t offset, uint64_t length) {
    auto it = space_list.begin();

    while (it != space_list.end()) {
        if (it->offset > offset + length)
            return;

        if (it->offset >= offset && it->offset + it->length <= offset + length) { // remove entry entirely
            auto it2 = it;

            it2++;
            space_list.erase(it);
            it = it2;
            continue;
        } else if (offset + length > it->offset && offset + length < it->offset + it->length) {
            if (offset > it->offset) { // cut out hole
                space_list.insert(it, space(it->offset, offset - it->offset));

                it->length = it->offset + it->length - offset - length;
                it->offset = offset + length;

                return;
            } else { // remove start of entry
                it->length -= offset + length - it->offset;
                it->offset = offset + length;
            }
        } else if (offset > it->offset && offset < it->offset + it->length) // remove end of entry
            it->length = offset - it->offset;

        it++;
    }
}

static void remove_superblocks(chunk& c) {
    unsigned int i = 0;

    // FIXME - DUP

    while (superblock_addrs[i] != 0) {
        if (c.disk_start + c.length > superblock_addrs[i] && c.disk_start < superblock_addrs[i] + stripe_length) {
            uint64_t start = max(c.offset, superblock_addrs[i] - c.disk_start + c.offset);
            uint64_t end = min(c.offset + c.length, superblock_addrs[i] + stripe_length - c.disk_start + c.offset);

            space_list_remove(c.space_list, start, end - start);
        }

        i++;
    }
}

static void create_data_chunks(ntfs& dev, const string& bmpdata) {
    uint64_t cluster_size = (uint64_t)dev.boot_sector->BytesPerSector * (uint64_t)dev.boot_sector->SectorsPerCluster;
    uint64_t addr = 0;

    // FIXME - make sure clusters_per_chunk is multiple of 8

    string_view bdsv = bmpdata;

    while (bdsv.length() > 0 && addr < device_size) {
        uint64_t chunk_length = min(device_size - addr, data_chunk_size);
        uint64_t clusters_per_chunk = chunk_length / cluster_size;
        string_view csv = bdsv.substr(0, (size_t)(clusters_per_chunk / 8));
        size_t len = csv.length();
        uint64_t run_start = 0, pos = 0;
        bool set = false;
        list<space> used;

        if (chunk_length % stripe_length != 0)
            chunk_length -= chunk_length % stripe_length;

        // FIXME - do by uint64_t if 64-bit processor?
        while (csv.size() >= sizeof(uint32_t)) {
            auto v = *(uint32_t*)csv.data();

            if ((!set && v == 0) || (set && v == 0xffffffff)) {
                pos += sizeof(uint32_t) * 8;
                csv = csv.substr(sizeof(uint32_t));
                continue;
            }

            if (!set && v == 0xffffffff) {
                run_start = pos;
                set = true;
                pos += sizeof(uint32_t) * 8;
            } else if (set && v == 0) {
                if (pos != run_start)
                    used.emplace_back(run_start, pos - run_start);

                set = false;
                pos += sizeof(uint32_t) * 8;
            } else {
                for (unsigned int i = 0; i < sizeof(uint32_t) * 8; i++) {
                    if (v & 1) {
                        if (!set) {
                            run_start = pos;
                            set = true;
                        }
                    } else {
                        if (set) {
                            if (pos != run_start)
                                used.emplace_back(run_start, pos - run_start);

                            set = false;
                        }
                    }

                    v >>= 1;
                    pos++;
                }
            }

            csv = csv.substr(sizeof(uint32_t));
        }

        while (!csv.empty()) {
            auto v = *(uint8_t*)csv.data();

            if ((!set && v == 0) || (set && v == 0xff)) {
                pos++;
                csv = csv.substr(1);
                continue;
            }

            if (!set && v == 0xff) {
                run_start = pos;
                set = true;
                pos += 8;
            } else if (set && v == 0) {
                if (pos != run_start)
                    used.emplace_back(run_start, pos - run_start);

                set = false;
                pos += 8;
            } else {
                for (unsigned int i = 0; i < 8; i++) {
                    if (v & 1) {
                        if (!set) {
                            run_start = pos;
                            set = true;
                        }
                    } else {
                        if (set) {
                            if (pos != run_start)
                                used.emplace_back(run_start, pos - run_start);

                            set = false;
                        }
                    }

                    v >>= 1;
                    pos++;
                }
            }

            csv = csv.substr(1);
        }

        if (set && run_start != pos)
            used.emplace_back(run_start, pos - run_start);

        if (!used.empty()) {
            space_list_remove(space_list, addr, chunk_length);
            chunks.emplace_back(addr + chunk_virt_offset, chunk_length, addr, BLOCK_FLAG_DATA);

            auto& c = chunks.back();
            uint64_t last = 0;

            for (const auto& u : used) {
                if (u.offset > last)
                    c.space_list.emplace_back(c.offset + (last * cluster_size), (u.offset - last) * cluster_size);

                last = u.offset + u.length;
            }

            if (last * cluster_size < chunk_length)
                c.space_list.emplace_back(c.offset + (last * cluster_size), chunk_length - (last * cluster_size));

            remove_superblocks(c);
        }

        addr += data_chunk_size;
        bdsv = bdsv.substr(len);
    }
}

static void add_item(root& r, uint64_t obj_id, uint8_t obj_type, uint64_t offset, const void* data, uint16_t len) {
    auto ret = r.items.emplace(KEY{obj_id, obj_type, offset}, tree_item{});

    if (!ret.second)
        throw formatted_error("Could not insert entry ({:x}, {:x}, {:x}) into root items list.", obj_id, obj_type, offset);

    auto& it = ret.first->second;

    new (&it) tree_item(data, len);
}

static void add_chunk(root& chunk_root, root& devtree_root, root& extent_root, const chunk& c) {
    chunk_item_one_stripe ci1s;
    DEV_EXTENT de;
    BLOCK_GROUP_ITEM bgi;

    memset(&ci1s, 0, sizeof(chunk_item_one_stripe));

    ci1s.chunk_item.size = c.length;
    ci1s.chunk_item.root_id = BTRFS_ROOT_EXTENT;
    ci1s.chunk_item.stripe_length = 0x10000;
    ci1s.chunk_item.type = c.type;
    ci1s.chunk_item.opt_io_alignment = 0x10000;
    ci1s.chunk_item.opt_io_width = 0x10000;
    ci1s.chunk_item.sector_size = 0x1000; // FIXME - get from superblock
    ci1s.chunk_item.num_stripes = 1;
    ci1s.chunk_item.sub_stripes = 1;
    ci1s.stripe.dev_id = 1;
    ci1s.stripe.offset = c.disk_start;
    ci1s.stripe.dev_uuid = dev_uuid;

    add_item(chunk_root, 0x100, TYPE_CHUNK_ITEM, c.offset, &ci1s, sizeof(ci1s));

    de.chunktree = BTRFS_ROOT_CHUNK;
    de.objid = 0x100;
    de.address = c.offset;
    de.length = c.length;
    de.chunktree_uuid = chunk_uuid;

    add_item(devtree_root, 1, TYPE_DEV_EXTENT, c.disk_start, &de, sizeof(DEV_EXTENT));

    bgi.chunk_tree = 0x100;
    bgi.flags = c.type;
    // bgi.used gets set in update_extent_root

    add_item(extent_root, c.offset, TYPE_BLOCK_GROUP_ITEM, c.length, &bgi, sizeof(BLOCK_GROUP_ITEM));
}

static uint64_t allocate_metadata(uint64_t r, root& extent_root, uint8_t level) {
    bool system_chunk = r == BTRFS_ROOT_CHUNK;
    uint64_t chunk_size, disk_offset;
    bool found = false;
    metadata_item mi;

    mi.extent_item.refcount = 1;
    mi.extent_item.generation = 1;
    mi.extent_item.flags = EXTENT_ITEM_TREE_BLOCK;
    mi.type = TYPE_TREE_BLOCK_REF;
    mi.tbr.offset = r;

    for (auto& c : chunks) {
        if ((system_chunk && c.type & BLOCK_FLAG_SYSTEM) || (!system_chunk && c.type & BLOCK_FLAG_METADATA)) {
            for (auto it = c.space_list.begin(); it != c.space_list.end(); it++) {
                if (it->length >= tree_size) {
                    uint64_t addr = it->offset;

                    if (it->length == tree_size)
                        c.space_list.erase(it);
                    else {
                        it->offset += tree_size;
                        it->length -= tree_size;
                    }

                    c.used += tree_size;

                    add_item(extent_root, addr, TYPE_METADATA_ITEM, level, &mi, sizeof(metadata_item));

                    return addr;
                }
            }
        }
    }

    // create new chunk

    chunks_changed = true;

    if (system_chunk)
        chunk_size = 32 * 1024 * 1024;
    else
        chunk_size = 128 * 1024 * 1024; // FIXME

    for (const auto& s : space_list) {
        if (s.length >= chunk_size) {
            disk_offset = s.offset;
            space_list_remove(space_list, disk_offset, chunk_size);
            found = true;
            break;
        }
    }

    if (!found)
        throw formatted_error("Could not find enough space to create new chunk.");

    chunks.emplace_back(disk_offset + chunk_virt_offset, chunk_size, disk_offset, system_chunk ? BLOCK_FLAG_SYSTEM : BLOCK_FLAG_METADATA);

    chunk& c = chunks.back();

    c.space_list.emplace_back(c.offset, c.length);

    remove_superblocks(c);

    for (auto it = c.space_list.begin(); it != c.space_list.end(); it++) {
        if (it->length >= tree_size) {
            uint64_t addr = it->offset;

            if (it->length == tree_size)
                c.space_list.erase(it);
            else {
                it->offset += tree_size;
                it->length -= tree_size;
            }

            c.used = tree_size;

            add_item(extent_root, addr, TYPE_METADATA_ITEM, level, &mi, sizeof(metadata_item));

            return addr;
        }
    }

    throw formatted_error("Could not allocate metadata address");
}

static uint64_t allocate_data(uint64_t length, bool change_used) {
    uint64_t disk_offset;
    bool found = false;

    for (auto& c : chunks) {
        if (c.type & BLOCK_FLAG_DATA) {
            for (auto it = c.space_list.begin(); it != c.space_list.end(); it++) {
                if (it->length >= length) {
                    uint64_t addr = it->offset;

                    if (it->length == length)
                        c.space_list.erase(it);
                    else {
                        it->offset += length;
                        it->length -= length;
                    }

                    if (change_used)
                        c.used += length;

                    return addr;
                }
            }
        }
    }

    // create new chunk

    chunks_changed = true;

    for (const auto& s : space_list) {
        if (s.length >= data_chunk_size) {
            disk_offset = s.offset;
            space_list_remove(space_list, disk_offset, data_chunk_size);
            found = true;
            break;
        }
    }

    if (!found)
        throw formatted_error("Could not find enough space to create new chunk.");

    chunks.emplace_back(disk_offset + chunk_virt_offset, data_chunk_size, disk_offset, BLOCK_FLAG_DATA);

    chunk& c = chunks.back();

    c.space_list.emplace_back(c.offset, c.length);

    remove_superblocks(c);

    for (auto it = c.space_list.begin(); it != c.space_list.end(); it++) {
        if (it->length >= length) {
            uint64_t addr = it->offset;

            if (it->length == length)
                c.space_list.erase(it);
            else {
                it->offset += length;
                it->length -= length;
            }

            if (change_used)
                c.used = length;

            return addr;
        }
    }

    throw formatted_error("Could not allocate data address");
}

static void calc_tree_hash(tree_header* th, enum btrfs_csum_type csum_type) {
    switch (csum_type) {
        case btrfs_csum_type::crc32c:
            *(uint32_t*)th->csum = ~calc_crc32c(0xffffffff, (uint8_t*)&th->fs_uuid, tree_size - (uint32_t)sizeof(th->csum));
            break;

        case btrfs_csum_type::xxhash:
            *(uint64_t*)th->csum = XXH64((uint8_t*)&th->fs_uuid, tree_size - sizeof(th->csum), 0);
            break;

        case btrfs_csum_type::sha256:
            calc_sha256((uint8_t*)th, &th->fs_uuid, tree_size - sizeof(th->csum));
            break;

        case btrfs_csum_type::blake2:
            blake2b(th, 32, &th->fs_uuid, tree_size - sizeof(th->csum));
            break;

        default:
            break;
    }
}

void root::create_trees(root& extent_root, enum btrfs_csum_type csum_type) {
    uint32_t space_left, num_items;
    string buf;
    tree_header* th;

    buf.resize(tree_size);

    memset(buf.data(), 0, tree_size);
    space_left = tree_size - (uint32_t)sizeof(tree_header);
    num_items = 0;

    th = (tree_header*)buf.data();
    th->fs_uuid = fs_uuid;
    th->flags = HEADER_FLAG_MIXED_BACKREF | HEADER_FLAG_WRITTEN;
    th->chunk_tree_uuid = chunk_uuid;
    th->generation = 1;
    th->tree_id = id;
    th->level = 0;

    {
        auto ln = (leaf_node*)((uint8_t*)buf.data() + sizeof(tree_header));
        uint32_t data_off = tree_size - (uint32_t)sizeof(tree_header);

        for (const auto& i : items) {
            if (sizeof(leaf_node) + i.second.len > space_left) { // tree complete, add to list
                if (!old_addresses.empty()) {
                    th->address = old_addresses.front();
                    old_addresses.pop_front();
                } else {
                    th->address = allocate_metadata(id, extent_root, th->level);
                    allocations_done = true;
                }

                addresses.push_back(th->address);
                th->num_items = num_items;

                calc_tree_hash(th, csum_type);

                trees.push_back(buf);
                metadata_size += tree_size;

                memset(buf.data(), 0, tree_size);

                th->fs_uuid = fs_uuid;
                th->flags = HEADER_FLAG_MIXED_BACKREF | HEADER_FLAG_WRITTEN;
                th->chunk_tree_uuid = chunk_uuid;
                th->generation = 1;
                th->tree_id = id;

                space_left = data_off = tree_size - (uint32_t)sizeof(tree_header);
                num_items = 0;
                ln = (leaf_node*)((uint8_t*)buf.data() + sizeof(tree_header));
            }

            if (sizeof(leaf_node) + i.second.len + sizeof(tree_header) > tree_size)
                throw formatted_error("Item too large for tree.");

            ln->key = i.first;
            ln->size = i.second.len;

            if (i.second.len != 0) {
                data_off -= i.second.len;
                memcpy((uint8_t*)buf.data() + sizeof(tree_header) + data_off, i.second.data, i.second.len);
            }

            ln->offset = data_off;

            ln++;

            num_items++;
            space_left -= (uint32_t)sizeof(leaf_node) + i.second.len;
        }
    }

    if (num_items > 0 || items.size() == 0) { // flush remaining tree
        if (!old_addresses.empty()) {
            th->address = old_addresses.front();
            old_addresses.pop_front();
        } else {
            th->address = allocate_metadata(id, extent_root, th->level);
            allocations_done = true;
        }

        addresses.push_back(th->address);
        th->num_items = num_items;

        calc_tree_hash(th, csum_type);

        trees.push_back(buf);
        metadata_size += tree_size;
    }

    level = 0;

    if (trees.size() == 1) { // no internal trees needed
        tree_addr = ((tree_header*)trees.back().data())->address;
        return;
    }

    // create internal trees if necessary

    do {
        unsigned int trees_added = 0;

        level++;

        memset(buf.data(), 0, tree_size);

        th = (tree_header*)buf.data();
        th->fs_uuid = fs_uuid;
        th->flags = HEADER_FLAG_MIXED_BACKREF | HEADER_FLAG_WRITTEN;
        th->chunk_tree_uuid = chunk_uuid;
        th->generation = 1;
        th->tree_id = id;
        th->level = level;

        num_items = 0;
        space_left = tree_size - (uint32_t)sizeof(tree_header);

        auto in = (internal_node*)((uint8_t*)buf.data() + sizeof(tree_header));

        for (const auto& t : trees) {
            auto th2 = (tree_header*)t.data();

            if (th2->level >= level)
                break;

            if (th2->level < level - 1)
                continue;

            if (sizeof(internal_node) > space_left) { // tree complete, add to list
                if (!old_addresses.empty()) {
                    th->address = old_addresses.front();
                    old_addresses.pop_front();
                } else {
                    th->address = allocate_metadata(id, extent_root, th->level);
                    allocations_done = true;
                }

                addresses.push_back(th->address);
                th->num_items = num_items;

                calc_tree_hash(th, csum_type);

                trees.push_back(buf);
                metadata_size += tree_size;

                memset(buf.data(), 0, tree_size);

                th->fs_uuid = fs_uuid;
                th->flags = HEADER_FLAG_MIXED_BACKREF | HEADER_FLAG_WRITTEN;
                th->chunk_tree_uuid = chunk_uuid;
                th->generation = 1;
                th->tree_id = id;
                th->level = level;

                space_left = tree_size - (uint32_t)sizeof(tree_header);
                num_items = 0;
                in = (internal_node*)((uint8_t*)buf.data() + sizeof(tree_header));

                trees_added++;
            }

            auto ln = (leaf_node*)((uint8_t*)t.data() + sizeof(tree_header));

            in->key = ln->key;
            in->address = th2->address;
            in->generation = 1;

            in++;

            num_items++;
            space_left -= (uint32_t)sizeof(internal_node);
        }

        if (num_items > 0) { // flush remaining tree
            if (!old_addresses.empty()) {
                th->address = old_addresses.front();
                old_addresses.pop_front();
            } else {
                th->address = allocate_metadata(id, extent_root, th->level);
                allocations_done = true;
            }

            addresses.push_back(th->address);
            th->num_items = num_items;

            calc_tree_hash(th, csum_type);

            trees.push_back(buf);
            metadata_size += tree_size;

            trees_added++;
        }

        if (trees_added == 1)
            break;
    } while (true);

    tree_addr = ((tree_header*)trees.back().data())->address;

    // FIXME - make sure level of METADATA_ITEMs is correct
}

void root::write_trees(ntfs& dev) {
    for (const auto& t : trees) {
        auto th = (tree_header*)t.data();
        uint64_t addr = th->address;
        bool found = false;

        for (const auto& c : chunks) {
            if (c.offset <= addr && c.offset + c.length >= addr + tree_size) {
                uint64_t physaddr = th->address - c.offset + c.disk_start;

                // FIXME - handle DUP

                dev.seek(physaddr);
                dev.write(t.data(), t.length());

                found = true;
                break;
            }
        }

        if (!found)
            throw formatted_error("Could not find chunk containing address."); // FIXME - include number
    }
}

static void set_volume_label(superblock& sb, ntfs& dev) {
    try {
        ntfs_file vol_file(dev, NTFS_VOLUME_INODE);

        auto vnw = vol_file.read(0, 0, ntfs_attribute::VOLUME_NAME);

        if (vnw.empty())
            return;

        wstring_convert<codecvt_utf8_utf16<char16_t>, char16_t> convert;

        auto vn = convert.to_bytes((char16_t*)vnw.data(), (char16_t*)&vnw[vnw.length()]);

        if (vn.length() > MAX_LABEL_SIZE) {
            vn = vn.substr(0, MAX_LABEL_SIZE);

            // remove whole code point
            while (!vn.empty() && vn[vn.length() - 1] & 0x80) {
                vn.pop_back();
            }

            cerr << "Truncating volume label to \"" << vn << "\"" << endl;
        }

        // FIXME - check label doesn't contain slash or backslash

        if (vn.empty())
            return;

        memcpy(sb.label, vn.data(), vn.length());
    } catch (const exception& e) { // shouldn't be fatal
        cerr << "Error while setting volume label: " << e.what() << endl;
    }
}

static void write_superblocks(ntfs& dev, root& chunk_root, root& root_root, enum btrfs_compression compression,
                              enum btrfs_csum_type csum_type) {
    uint32_t sector_size = 0x1000; // FIXME
    string buf;
    unsigned int i;
    uint32_t sys_chunk_size;
    uint64_t total_used;

    buf.resize((size_t)sector_align(sizeof(superblock), sector_size));
    auto& sb = *(superblock*)buf.data();

    memset(buf.data(), 0, buf.length());

    sys_chunk_size = 0;
    for (const auto& c : chunk_root.items) {
        if (c.first.obj_type == TYPE_CHUNK_ITEM) {
            auto& ci = *(CHUNK_ITEM*)c.second.data;

            if (ci.type & BLOCK_FLAG_SYSTEM) {
                sys_chunk_size += sizeof(KEY);
                sys_chunk_size += c.second.len;
            }
        }
    }

    if (sys_chunk_size > SYS_CHUNK_ARRAY_SIZE)
        throw formatted_error("System chunk list was too long ({} > {}).", sys_chunk_size, SYS_CHUNK_ARRAY_SIZE);

    total_used = 0;

    for (const auto& c : chunks) {
        total_used += c.used;
    }

    sb.uuid = fs_uuid;
    sb.magic = BTRFS_MAGIC;
    sb.generation = 1;
    sb.root_tree_addr = root_root.tree_addr;
    sb.chunk_tree_addr = chunk_root.tree_addr;
    sb.total_bytes = device_size;
    sb.bytes_used = total_used;
    sb.root_dir_objectid = BTRFS_ROOT_TREEDIR;
    sb.num_devices = 1;
    sb.sector_size = sector_size;
    sb.node_size = tree_size;
    sb.leaf_size = tree_size;
    sb.stripe_size = sector_size;
    sb.n = sys_chunk_size;
    sb.chunk_root_generation = 1;
    sb.incompat_flags = BTRFS_INCOMPAT_FLAGS_MIXED_BACKREF | BTRFS_INCOMPAT_FLAGS_BIG_METADATA | BTRFS_INCOMPAT_FLAGS_EXTENDED_IREF |
                        BTRFS_INCOMPAT_FLAGS_SKINNY_METADATA | BTRFS_INCOMPAT_FLAGS_NO_HOLES;
    sb.csum_type = csum_type;
    sb.root_level = root_root.level;
    sb.chunk_root_level = chunk_root.level;

    if (compression == btrfs_compression::lzo)
        sb.incompat_flags |= BTRFS_INCOMPAT_FLAGS_COMPRESS_LZO;
    else if (compression == btrfs_compression::zstd)
        sb.incompat_flags |= BTRFS_INCOMPAT_FLAGS_COMPRESS_ZSTD;

    set_volume_label(sb, dev);

    for (const auto& c : chunk_root.items) {
        if (c.first.obj_type == TYPE_DEV_ITEM) {
            memcpy(&sb.dev_item, c.second.data, sizeof(DEV_ITEM));
            break;
        }
    }

    sb.uuid_tree_generation = 1;

    {
        uint8_t* ptr = sb.sys_chunk_array;

        for (const auto& c : chunk_root.items) {
            if (c.first.obj_type == TYPE_CHUNK_ITEM) {
                auto& ci = *(CHUNK_ITEM*)c.second.data;

                if (ci.type & BLOCK_FLAG_SYSTEM) {
                    auto& key = *(KEY*)ptr;

                    key = c.first;

                    ptr += sizeof(KEY);

                    memcpy(ptr, c.second.data, c.second.len);

                    ptr += c.second.len;
                }
            }
        }
    }

    i = 0;
    while (superblock_addrs[i] != 0) {
        if (superblock_addrs[i] > device_size - buf.length())
            return;

        sb.sb_phys_addr = superblock_addrs[i];

        switch (csum_type) {
            case btrfs_csum_type::crc32c:
                *(uint32_t*)sb.checksum = ~calc_crc32c(0xffffffff, (uint8_t*)&sb.uuid, sizeof(superblock) - sizeof(sb.checksum));
                break;

            case btrfs_csum_type::xxhash:
                *(uint64_t*)sb.checksum = XXH64(&sb.uuid, sizeof(superblock) - sizeof(sb.checksum), 0);
                break;

            case btrfs_csum_type::sha256:
                calc_sha256((uint8_t*)&sb, &sb.uuid, sizeof(superblock) - sizeof(sb.checksum));
                break;

            case btrfs_csum_type::blake2:
                blake2b(&sb, 32, &sb.uuid, sizeof(superblock) - sizeof(sb.checksum));
                break;

            default:
                break;
        }

        dev.seek(superblock_addrs[i]);
        dev.write(buf.data(), buf.length());

        i++;
    }
}

static void add_dev_item(root& chunk_root) {
    DEV_ITEM di;
    uint32_t sector_size = 0x1000; // FIXME - get from superblock

    memset(&di, 0, sizeof(DEV_ITEM));
    di.dev_id = 1;
    di.num_bytes = device_size;
    //uint64_t bytes_used; // FIXME
    di.optimal_io_align = sector_size;
    di.optimal_io_width = sector_size;
    di.minimal_io_size = sector_size;
    di.device_uuid = dev_uuid;
    di.fs_uuid = fs_uuid;

    add_item(chunk_root, 1, TYPE_DEV_ITEM, 1, &di, sizeof(DEV_ITEM));
}

static void add_to_root_root(const root& r, root& root_root) {
    ROOT_ITEM ri;

    memset(&ri, 0, sizeof(ROOT_ITEM));

    ri.inode.generation = 1;
    ri.inode.st_blocks = tree_size;
    ri.inode.st_size = 3;
    ri.inode.st_nlink = 1;
    ri.inode.st_mode = __S_IFDIR | S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
    ri.generation = 1;
    ri.objid = (r.id == BTRFS_ROOT_FSTREE || r.id >= 0x100) ? SUBVOL_ROOT_INODE : 0;
    ri.flags = r.readonly ? BTRFS_SUBVOL_READONLY : 0;
    ri.num_references = 1;
    ri.generation2 = 1;

    if (r.id == image_subvol_id)
        ri.uuid = subvol_uuid;

    // block_number, bytes_used, and root_level are set in update_root_root

    add_item(root_root, r.id, TYPE_ROOT_ITEM, 0, &ri, sizeof(ROOT_ITEM));
}

static void update_root_root(root& root_root, enum btrfs_csum_type csum_type) {
    for (auto& t : root_root.trees) {
        auto th = (tree_header*)t.data();

        if (th->level > 0)
            return;

        auto ln = (leaf_node*)((uint8_t*)t.data() + sizeof(tree_header));
        bool changed = true;

        for (unsigned int i = 0; i < th->num_items; i++) {
            if (ln[i].key.obj_type == TYPE_ROOT_ITEM) {
                auto ri = (ROOT_ITEM*)((uint8_t*)t.data() + sizeof(tree_header) + ln[i].offset);

                for (const auto& r : roots) {
                    if (r.id == ln[i].key.obj_id) {
                        ri->block_number = r.tree_addr;
                        ri->root_level = r.level;
                        ri->bytes_used = r.metadata_size;

                        changed = true;
                    }
                }
            }
        }

        if (changed)
            calc_tree_hash(th, csum_type);
    }
}

static void add_dev_stats(root& r) {
    uint64_t ds[5];

    memset(ds, 0, sizeof(ds));

    add_item(r, 0, TYPE_DEV_STATS, 1, &ds, sizeof(ds));
}

static BTRFS_UUID generate_uuid(default_random_engine& gen) {
    BTRFS_UUID uuid;
    uniform_int_distribution<unsigned int> dist(0,0xffffffff);

    for (unsigned int i = 0; i < 4; i++) {
        *(uint32_t*)&uuid.uuid[i * sizeof(uint32_t)] = dist(gen);
    }

    return uuid;
}

static void update_extent_root(root& extent_root, enum btrfs_csum_type csum_type) {
    for (auto& t : extent_root.trees) {
        auto th = (tree_header*)t.data();

        if (th->level > 0)
            return;

        auto ln = (leaf_node*)((uint8_t*)t.data() + sizeof(tree_header));
        bool changed = true;

        for (unsigned int i = 0; i < th->num_items; i++) {
            if (ln[i].key.obj_type == TYPE_BLOCK_GROUP_ITEM) {
                auto bgi = (BLOCK_GROUP_ITEM*)((uint8_t*)t.data() + sizeof(tree_header) + ln[i].offset);

                for (const auto& c : chunks) {
                    if (c.offset == ln[i].key.obj_id) {
                        bgi->used = c.used;

                        changed = true;
                    }
                }
            }
        }

        if (changed)
            calc_tree_hash(th, csum_type);
    }
}

static void add_inode_ref(root& r, uint64_t inode, uint64_t parent, uint64_t index, const string_view& name) {
    if (r.items.count(KEY{inode, TYPE_INODE_REF, parent}) != 0) { // collision, append to the end
        auto& old = r.items.at(KEY{inode, TYPE_INODE_REF, parent});

        size_t irlen = offsetof(INODE_REF, name[0]) + name.length() + old.len;

        // FIXME - check if too long for tree, and create INODE_EXTREF instead

        auto buf = malloc(irlen);
        if (!buf)
            throw bad_alloc();

        memcpy(buf, old.data, old.len);

        auto& ir = *(INODE_REF*)((uint8_t*)buf + old.len);

        ir.index = index;
        ir.n = (uint16_t)name.length();
        memcpy(ir.name, name.data(), name.length());

        old.data = buf;
        old.len = (uint16_t)irlen;

        return;
    }

    vector<uint8_t> buf(offsetof(INODE_REF, name[0]) + name.length());
    auto& ir = *(INODE_REF*)buf.data();

    ir.index = index;
    ir.n = (uint16_t)name.length();
    memcpy(ir.name, name.data(), name.length());

    add_item(r, inode, TYPE_INODE_REF, parent, &ir, (uint16_t)buf.size());
}

static void populate_fstree(root& r) {
    INODE_ITEM ii;

    memset(&ii, 0, sizeof(INODE_ITEM));

    ii.generation = 1;
    ii.transid = 1;
    ii.st_nlink = 1;
    ii.st_mode = __S_IFDIR | S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
    ii.sequence = 1;

    add_item(r, SUBVOL_ROOT_INODE, TYPE_INODE_ITEM, 0, &ii, sizeof(INODE_ITEM));

    add_inode_ref(r, SUBVOL_ROOT_INODE, SUBVOL_ROOT_INODE, 0, "..");
}

static void update_chunk_root(root& chunk_root, enum btrfs_csum_type csum_type) {
    for (auto& t : chunk_root.trees) {
        auto th = (tree_header*)t.data();

        if (th->level > 0)
            return;

        auto ln = (leaf_node*)((uint8_t*)t.data() + sizeof(tree_header));

        for (unsigned int i = 0; i < th->num_items; i++) {
            if (ln[i].key.obj_id == 1 && ln[i].key.obj_type == TYPE_DEV_ITEM && ln[i].key.offset == 1) {
                auto di = (DEV_ITEM*)((uint8_t*)t.data() + sizeof(tree_header) + ln[i].offset);

                di->bytes_used = 0;

                for (const auto& c : chunks) {
                    di->bytes_used += c.length;
                }

                calc_tree_hash(th, csum_type);

                return;
            }
        }
    }
}

static root& add_image_subvol(root& root_root, root& fstree_root) {
    static const char subvol_name[] = "image";

    roots.emplace_back(image_subvol_id);
    root& r = roots.back();

    r.readonly = true;

    // add ROOT_REF and ROOT_BACKREF

    {
        size_t rrlen = offsetof(ROOT_REF, name[0]) + sizeof(subvol_name) - 1;
        auto rr = (ROOT_REF*)malloc(rrlen);
        if (!rr)
            throw bad_alloc();

        try {
            rr->dir = SUBVOL_ROOT_INODE;
            rr->index = 2;
            rr->n = sizeof(subvol_name) - 1;
            memcpy(rr->name, subvol_name, sizeof(subvol_name) - 1);

            add_item(root_root, BTRFS_ROOT_FSTREE, TYPE_ROOT_REF, image_subvol_id, rr, (uint16_t)rrlen);
            add_item(root_root, image_subvol_id, TYPE_ROOT_BACKREF, BTRFS_ROOT_FSTREE, rr, (uint16_t)rrlen);
        } catch (...) {
            free(rr);
            throw;
        }

        free(rr);
    }

    // add DIR_ITEM and DIR_INDEX

    {
        size_t dilen = offsetof(DIR_ITEM, name[0]) + sizeof(subvol_name) - 1;
        auto di = (DIR_ITEM*)malloc(dilen);
        if (!di)
            throw bad_alloc();

        try {
            uint32_t hash;

            di->key.obj_id = image_subvol_id;
            di->key.obj_type = TYPE_ROOT_ITEM;
            di->key.offset = 0xffffffffffffffff;
            di->transid = 1;
            di->m = 0;
            di->n = sizeof(subvol_name) - 1;
            di->type = BTRFS_TYPE_DIRECTORY;
            memcpy(di->name, subvol_name, sizeof(subvol_name) - 1);

            hash = calc_crc32c(0xfffffffe, (const uint8_t*)subvol_name, sizeof(subvol_name) - 1);

            add_item(fstree_root, SUBVOL_ROOT_INODE, TYPE_DIR_ITEM, hash, di, (uint16_t)dilen);
            add_item(fstree_root, SUBVOL_ROOT_INODE, TYPE_DIR_INDEX, 2, di, (uint16_t)dilen);
        } catch (...) {
            free(di);
            throw;
        }

        free(di);
    }

    // increase st_size in parent dir
    if (fstree_root.dir_size.count(SUBVOL_ROOT_INODE) == 0)
        fstree_root.dir_size[SUBVOL_ROOT_INODE] = (sizeof(subvol_name) - 1) * 2;
    else
        fstree_root.dir_size.at(SUBVOL_ROOT_INODE) += (sizeof(subvol_name) - 1) * 2;

    populate_fstree(r);

    return r;
}

static void create_image(root& r, ntfs& dev, const runs_t& runs, uint64_t inode) {
    INODE_ITEM ii;
    uint64_t cluster_size = (uint64_t)dev.boot_sector->BytesPerSector * (uint64_t)dev.boot_sector->SectorsPerCluster;

    // add INODE_ITEM

    memset(&ii, 0, sizeof(INODE_ITEM));

    ii.generation = 1;
    ii.transid = 1;
    ii.st_size = orig_device_size;
    ii.st_nlink = 1;
    ii.st_mode = __S_IFREG | S_IRUSR | S_IWUSR;
    ii.sequence = 1;

    // FIXME - use current time for the following
//     BTRFS_TIME st_atime;
//     BTRFS_TIME st_ctime;
//     BTRFS_TIME st_mtime;
//     BTRFS_TIME otime;

    for (const auto& rs : runs) {
        for (const auto& run : rs.second) {
            if (!run.relocated && !run.not_in_img)
                ii.st_blocks += run.length * cluster_size;
        }
    }

    add_item(r, inode, TYPE_INODE_ITEM, 0, &ii, sizeof(INODE_ITEM));

    // add DIR_ITEM and DIR_INDEX

    {
        vector<uint8_t> buf(offsetof(DIR_ITEM, name[0]) + sizeof(image_filename) - 1);
        auto& di = *(DIR_ITEM*)buf.data();

        di.key.obj_id = inode;
        di.key.obj_type = TYPE_INODE_ITEM;
        di.key.offset = 0;
        di.transid = 1;
        di.m = 0;
        di.n = sizeof(image_filename) - 1;
        di.type = BTRFS_TYPE_FILE;
        memcpy(di.name, image_filename, sizeof(image_filename) - 1);

        auto hash = calc_crc32c(0xfffffffe, (const uint8_t*)image_filename, sizeof(image_filename) - 1);

        add_item(r, SUBVOL_ROOT_INODE, TYPE_DIR_ITEM, hash, &di, (uint16_t)buf.size());
        add_item(r, SUBVOL_ROOT_INODE, TYPE_DIR_INDEX, 2, &di, (uint16_t)buf.size());
    }

    // add INODE_REF

    add_inode_ref(r, inode, SUBVOL_ROOT_INODE, 2, image_filename);

    // increase st_size in parent dir

    for (auto& it : r.items) {
        if (it.first.obj_id == SUBVOL_ROOT_INODE && it.first.obj_type == TYPE_INODE_ITEM) {
            auto ii2 = (INODE_ITEM*)it.second.data;

            ii2->st_size += (sizeof(image_filename) - 1) * 2;
            break;
        }
    }

    // add extents

    vector<uint8_t> buf(offsetof(EXTENT_DATA, data[0]) + sizeof(EXTENT_DATA2));
    auto& ed = *(EXTENT_DATA*)buf.data();
    auto& ed2 = *(EXTENT_DATA2*)&ed.data;

    ed.generation = 1;
    ed.compression = btrfs_compression::none;
    ed.encryption = 0;
    ed.encoding = 0;
    ed.type = btrfs_extent_type::regular;

    for (const auto& rs : runs) {
        for (const auto& run : rs.second) {
            uint64_t addr;

            if (run.relocated || run.not_in_img)
                continue;

            ed.decoded_size = ed2.size = ed2.num_bytes = run.length * cluster_size;

            addr = run.offset * cluster_size;

            if (run.inode == dummy_inode) {
                for (const auto& reloc : relocs) {
                    if (reloc.old_start == run.offset) {
                        ed2.address = (reloc.new_start * cluster_size) + chunk_virt_offset;
                        break;
                    }
                }
            } else
                ed2.address = addr + chunk_virt_offset;

            ed2.offset = 0;

            add_item(r, inode, TYPE_EXTENT_DATA, addr, &ed, (uint16_t)buf.size());

            data_size += ed2.size;
        }
    }
}

template<typename T>
static void parse_bitmap(const string& bmpdata, list<T>& runs) {
    uint64_t run_start = 0, pos = 0;
    bool set = false;
    string_view bdsv = bmpdata;

    // FIXME - by 64-bits if 64-bit processor (use typedef for uint64_t/uint32_t?)

    while (bdsv.size() >= sizeof(uint32_t)) {
        auto v = *(uint32_t*)bdsv.data();

        if ((!set && v == 0) || (set && v == 0xffffffff)) {
            pos += sizeof(uint32_t) * 8;
            bdsv = bdsv.substr(sizeof(uint32_t));
            continue;
        }

        if (!set && v == 0xffffffff) {
            run_start = pos;
            set = true;
            pos += sizeof(uint32_t) * 8;
        } else if (set && v == 0) {
            if (pos != run_start)
                runs.emplace_back(run_start, pos - run_start);

            set = false;
            pos += sizeof(uint32_t) * 8;
        } else {
            for (unsigned int i = 0; i < sizeof(uint32_t) * 8; i++) {
                if (v & 1) {
                    if (!set) {
                        run_start = pos;
                        set = true;
                    }
                } else {
                    if (set) {
                        if (pos != run_start)
                            runs.emplace_back(run_start, pos - run_start);

                        set = false;
                    }
                }

                v >>= 1;
                pos++;
            }
        }

        bdsv = bdsv.substr(sizeof(uint32_t));
    }

    while (!bdsv.empty()) {
        auto v = *(uint8_t*)bdsv.data();

        if ((!set && v == 0) || (set && v == 0xff)) {
            pos++;
            bdsv = bdsv.substr(1);
            continue;
        }

        if (!set && v == 0xff) {
            run_start = pos;
            set = true;
            pos += 8;
        } else if (set && v == 0) {
            if (pos != run_start)
                runs.emplace_back(run_start, pos - run_start);

            set = false;
            pos += 8;
        } else {
            for (unsigned int i = 0; i < 8; i++) {
                if (v & 1) {
                    if (!set) {
                        run_start = pos;
                        set = true;
                    }
                } else {
                    if (set) {
                        if (pos != run_start)
                            runs.emplace_back(run_start, pos - run_start);

                        set = false;
                    }
                }

                v >>= 1;
                pos++;
            }
        }

        bdsv = bdsv.substr(1);
    }

    if (set && run_start != pos)
        runs.emplace_back(run_start, pos - run_start);

    // FIXME - remove any bits after end of volume
}

static void parse_data_bitmap(ntfs& dev, const string& bmpdata, runs_t& runs) {
    uint64_t run_start = 0, pos = 0;
    bool set = false;
    string_view bdsv = bmpdata;

    uint64_t clusters_per_chunk = data_chunk_size / ((uint64_t)dev.boot_sector->BytesPerSector * (uint64_t)dev.boot_sector->SectorsPerCluster);

    // FIXME - by 64-bits if 64-bit processor (use typedef for uint64_t/uint32_t?)

    auto add_run = [&]() {
        while (true) {
            uint64_t chunk = run_start / clusters_per_chunk;

            auto& r = runs[chunk];

            if (pos / clusters_per_chunk != chunk) {
                uint64_t len = clusters_per_chunk - (run_start % clusters_per_chunk);

                r.emplace_back(run_start, len);
                run_start += len;

                if (pos == run_start)
                    break;
            } else {
                r.emplace_back(run_start, pos - run_start);
                break;
            }
        }
    };

    while (bdsv.size() >= sizeof(uint32_t)) {
        auto v = *(uint32_t*)bdsv.data();

        if ((!set && v == 0) || (set && v == 0xffffffff)) {
            pos += sizeof(uint32_t) * 8;
            bdsv = bdsv.substr(sizeof(uint32_t));
            continue;
        }

        if (!set && v == 0xffffffff) {
            run_start = pos;
            set = true;
            pos += sizeof(uint32_t) * 8;
        } else if (set && v == 0) {
            if (pos != run_start)
                add_run();

            set = false;
            pos += sizeof(uint32_t) * 8;
        } else {
            for (unsigned int i = 0; i < sizeof(uint32_t) * 8; i++) {
                if (v & 1) {
                    if (!set) {
                        run_start = pos;
                        set = true;
                    }
                } else {
                    if (set) {
                        if (pos != run_start)
                            add_run();

                        set = false;
                    }
                }

                v >>= 1;
                pos++;
            }
        }

        bdsv = bdsv.substr(sizeof(uint32_t));
    }

    while (!bdsv.empty()) {
        auto v = *(uint8_t*)bdsv.data();

        if ((!set && v == 0) || (set && v == 0xff)) {
            pos++;
            bdsv = bdsv.substr(1);
            continue;
        }

        if (!set && v == 0xff) {
            run_start = pos;
            set = true;
            pos += 8;
        } else if (set && v == 0) {
            if (pos != run_start)
                add_run();

            set = false;
            pos += 8;
        } else {
            for (unsigned int i = 0; i < 8; i++) {
                if (v & 1) {
                    if (!set) {
                        run_start = pos;
                        set = true;
                    }
                } else {
                    if (set) {
                        if (pos != run_start)
                            add_run();

                        set = false;
                    }
                }

                v >>= 1;
                pos++;
            }
        }

        bdsv = bdsv.substr(1);
    }

    if (set && run_start != pos)
        add_run();

    // FIXME - remove any bits after end of volume
}

static BTRFS_TIME win_time_to_unix(int64_t time) {
    uint64_t l = (uint64_t)time - 116444736000000000ULL;
    BTRFS_TIME bt;

    bt.seconds = l / 10000000;
    bt.nanoseconds = (uint32_t)((l % 10000000) * 100);

    return bt;
}

static void link_inode(root& r, uint64_t inode, uint64_t dir, const string_view& name, uint8_t type) {
    uint64_t seq;

    // add DIR_ITEM and DIR_INDEX

    if (r.dir_seqs.count(dir) == 0)
        r.dir_seqs[dir] = 2;

    seq = r.dir_seqs.at(dir);

    {
        size_t dilen = offsetof(DIR_ITEM, name[0]) + name.length();
        auto di = (DIR_ITEM*)malloc(dilen);
        if (!di)
            throw bad_alloc();

        try {
            uint32_t hash;

            di->key.obj_id = inode;
            di->key.obj_type = TYPE_INODE_ITEM;
            di->key.offset = 0;
            di->transid = 1;
            di->m = 0;
            di->n = (uint16_t)name.length();
            di->type = type;
            memcpy(di->name, name.data(), name.length());

            hash = calc_crc32c(0xfffffffe, (const uint8_t*)name.data(), (uint32_t)name.length());

            if (r.items.count(KEY{dir, TYPE_DIR_ITEM, hash}) == 0)
                add_item(r, dir, TYPE_DIR_ITEM, hash, di, (uint16_t)dilen);
            else { // hash collision
                auto& ent = r.items.at(KEY{dir, TYPE_DIR_ITEM, hash});

                if (ent.len != 0) {
                    void* data = malloc(ent.len + dilen);

                    if (!data)
                        throw bad_alloc();

                    memcpy(data, ent.data, ent.len);
                    memcpy((uint8_t*)data + ent.len, di, dilen);

                    free(ent.data);

                    ent.data = data;
                    ent.len += (uint32_t)dilen;
                } else {
                    ent.data = malloc(dilen);
                    if (!ent.data)
                        throw bad_alloc();

                    ent.len = (uint32_t)dilen;
                    memcpy(ent.data, di, dilen);
                }
            }

            add_item(r, dir, TYPE_DIR_INDEX, seq, di, (uint16_t)dilen);
        } catch (...) {
            free(di);
            throw;
        }

        free(di);
    }

    // add INODE_REF

    add_inode_ref(r, inode, dir, seq, name);

    // increase st_size in parent dir

    if (r.dir_size.count(dir) == 0)
        r.dir_size[dir] = name.length() * 2;
    else
        r.dir_size.at(dir) += name.length() * 2;

    r.dir_seqs[dir]++;
}

static bool split_runs(const ntfs& dev, runs_t& runs, uint64_t offset, uint64_t length, uint64_t inode, uint64_t file_offset) {
    uint64_t clusters_per_chunk = data_chunk_size / ((uint64_t)dev.boot_sector->BytesPerSector * (uint64_t)dev.boot_sector->SectorsPerCluster);
    bool ret = false;

    while (true) {
        uint64_t chunk = offset / clusters_per_chunk;
        uint64_t length2 = min(length, clusters_per_chunk - (offset % clusters_per_chunk));

        if (runs.count(chunk) != 0) {
            auto& rl = runs[chunk];

            for (auto it = rl.begin(); it != rl.end(); it++) {
                auto& r = *it;

                if (r.offset > offset + length2)
                    break;

                if (offset + length2 > r.offset && offset < r.offset + r.length) {
                    if (offset >= r.offset && offset + length2 <= r.offset + r.length) { // cut out middle
                        if (offset > r.offset)
                            rl.emplace(it, r.offset, offset - r.offset);

                        rl.emplace(it, offset, length2, inode, file_offset, r.relocated);

                        if (offset + length2 < r.offset + r.length) {
                            r.length = r.offset + r.length - offset - length2;
                            r.offset = offset + length2;
                        } else
                            rl.erase(it);

                        ret = true;
                        break;
                    }

                    throw formatted_error("Error assigning space to file. This can occur if the space bitmap has become corrupted. Run chkdsk and try again.");
                }
            }
        }

        if (length2 == length)
            return ret;

        offset += length2;
        length -= length2;
    }
}

static void process_mappings(const ntfs& dev, uint64_t inode, list<mapping>& mappings, runs_t& runs) {
    uint64_t cluster_size = (uint64_t)dev.boot_sector->BytesPerSector * (uint64_t)dev.boot_sector->SectorsPerCluster;
    uint64_t clusters_per_chunk = data_chunk_size / cluster_size;
    list<mapping> mappings2;

    // avoid chunk boundaries

    for (const auto& m : mappings) {
        if (m.lcn == 0) // sparse
            continue;

        uint64_t chunk_start = m.lcn / clusters_per_chunk;
        uint64_t chunk_end = ((m.lcn + m.length) - 1) / clusters_per_chunk;

        if (chunk_end > chunk_start) {
            uint64_t start = m.lcn, vcn = m.vcn;

            do {
                uint64_t end = min((((start / clusters_per_chunk) + 1) * clusters_per_chunk), m.lcn + m.length);

                if (end == start)
                    break;

                mappings2.emplace_back(start, vcn, end - start);

                vcn += end - start;
                start = end;
            } while (true);
        } else
            mappings2.emplace_back(m.lcn, m.vcn, m.length);
    }

    mappings.clear();
    mappings.splice(mappings.begin(), mappings2);

    // change to avoid superblocks

    for (auto& r : relocs) {
        for (auto it = mappings.begin(); it != mappings.end(); it++) {
            auto& m = *it;

            if (m.lcn + m.length > r.old_start && m.lcn < r.old_start + r.length) {
                if (m.lcn >= r.old_start && m.lcn + m.length <= r.old_start + r.length) { // change whole mapping
                    if (r.old_start < m.lcn) { // reloc starts before mapping
                        for (auto& rs : runs) { // FIXME - optimize
                            for (auto it2 = rs.second.begin(); it2 != rs.second.end(); it2++) {
                                auto& r2 = *it2;

                                if (r2.offset == r.old_start) {
                                    rs.second.emplace(it2, r2.offset, m.lcn - r2.offset, dummy_inode);

                                    r2.length -= m.lcn - r2.offset;
                                    r2.offset = m.lcn;
                                }

                                if (r2.offset == r.new_start) {
                                    rs.second.emplace(it2, r2.offset, m.lcn - r.old_start, 0, 0, true);

                                    r2.offset += m.lcn - r.old_start;
                                    r2.length -= m.lcn - r.old_start;
                                }
                            }
                        }

                        relocs.emplace_back(r.old_start, m.lcn - r.old_start, r.new_start);

                        r.length -= m.lcn - r.old_start;
                        r.new_start += m.lcn - r.old_start;
                        r.old_start = m.lcn;
                    }

                    if (r.old_start + r.length > m.lcn + m.length) { // reloc goes beyond end of mapping
                        relocs.emplace_back(m.lcn + m.length, r.old_start + r.length - m.lcn - m.length,
                                            r.new_start + m.lcn + m.length - r.old_start);

                        r.length = m.lcn + m.length - r.old_start;

                        for (auto& rs : runs) { // FIXME - optimize
                            for (auto it2 = rs.second.begin(); it2 != rs.second.end(); it2++) {
                                auto& r2 = *it2;

                                if (r2.offset == r.old_start) {
                                    rs.second.emplace(it2, r.old_start, m.lcn + m.length - r.old_start, dummy_inode);

                                    r2.length -= m.lcn + m.length - r2.offset;
                                    r2.offset = m.lcn + m.length;
                                }

                                if (r2.offset == r.new_start) {
                                    rs.second.emplace(it2, r2.offset, m.lcn + m.length - r.old_start, 0, 0, true);

                                    r2.offset += m.lcn + m.length - r.old_start;
                                    r2.length -= m.lcn + m.length - r.old_start;
                                }
                            }
                        }
                    }

                    m.lcn -= r.old_start;
                    m.lcn += r.new_start;
                } else if (m.lcn <= r.old_start && m.lcn + m.length >= r.old_start + r.length) { // change middle
                    if (m.lcn < r.old_start) {
                        mappings.emplace(it, m.lcn, m.vcn, r.old_start - m.lcn);
                        m.vcn += r.old_start - m.lcn;
                        m.length -= r.old_start - m.lcn;
                        m.lcn = r.old_start;
                    }

                    if (m.lcn + m.length > r.old_start + r.length) {
                        mappings.emplace(it, r.new_start, m.vcn, r.length);

                        m.lcn = r.old_start + r.length;
                        m.length -= r.length;
                        m.vcn += r.length;
                    } else {
                        m.lcn -= r.old_start;
                        m.lcn += r.new_start;
                    }
                } else if (m.lcn < r.old_start && m.lcn + m.length <= r.old_start + r.length) { // change end
                    mappings.emplace(it, m.lcn, m.vcn, r.old_start - m.lcn);

                    m.vcn += r.old_start - m.lcn;
                    m.length -= r.old_start - m.lcn;
                    m.lcn = r.new_start;

                    if (r.length > m.length) {
                        relocs.emplace_back(r.old_start + m.length, r.length - m.length, r.new_start + m.length);

                        r.length = m.length;

                        for (auto& rs : runs) { // FIXME - optimize
                            bool found = false;

                            for (auto it2 = rs.second.begin(); it2 != rs.second.end(); it2++) {
                                auto& r2 = *it2;

                                if (r2.offset == r.old_start) {
                                    rs.second.emplace(it2, r2.offset, m.length, dummy_inode);

                                    r2.offset += m.length;
                                    r2.length -= m.length;

                                    found = true;
                                    break;
                                }
                            }

                            if (found)
                                break;
                        }
                    }
                } else if (m.lcn > r.old_start && m.lcn + m.length > r.old_start + r.length) { // change beginning
                    auto orig_r = r;

                    if (r.old_start < m.lcn) {
                        for (auto& rs : runs) { // FIXME - optimize
                            for (auto it2 = rs.second.begin(); it2 != rs.second.end(); it2++) {
                                auto& r2 = *it2;

                                if (r2.offset == r.old_start) {
                                    rs.second.emplace(it2, r2.offset, m.lcn - r2.offset, dummy_inode);

                                    r2.length -= m.lcn - r2.offset;
                                    r2.offset = m.lcn;
                                }

                                if (r2.offset == r.new_start) {
                                    rs.second.emplace(it2, r2.offset, m.lcn - r.old_start, 0, 0, true);

                                    r2.offset += m.lcn - r.old_start;
                                    r2.length -= m.lcn - r.old_start;
                                }
                            }
                        }

                        relocs.emplace_back(m.lcn, r.old_start + r.length - m.lcn, r.new_start + m.lcn - r.old_start);

                        r.length = m.lcn - r.old_start;
                    }

                    mappings.emplace(it, m.lcn - orig_r.old_start + orig_r.new_start, m.vcn, orig_r.old_start + orig_r.length - m.lcn);

                    m.vcn += orig_r.old_start + orig_r.length - m.lcn;
                    m.length -= orig_r.old_start + orig_r.length - m.lcn;
                    m.lcn = orig_r.old_start + orig_r.length;
                }
            }
        }
    }

    for (const auto& m : mappings) {
        split_runs(dev, runs, m.lcn, m.length, inode, m.vcn);
    }
}

static void set_xattr(root& r, uint64_t inode, const string_view& name, uint32_t hash, const string_view& data) {
    vector<uint8_t> buf(offsetof(DIR_ITEM, name[0]) + name.size() + data.size());
    auto& di = *(DIR_ITEM*)buf.data();

    di.key.obj_id = di.key.offset = 0;
    di.key.obj_type = 0;
    di.transid = 1;
    di.m = (uint16_t)data.size();
    di.n = (uint16_t)name.size();
    di.type = BTRFS_TYPE_EA;
    memcpy(di.name, name.data(), name.size());
    memcpy(di.name + name.size(), data.data(), data.size());

    add_item(r, inode, TYPE_XATTR_ITEM, hash, &di, (uint16_t)buf.size());
}

static void clear_line() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);

    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        DWORD written;

        SetConsoleCursorPosition(console, { 0, csbi.dwCursorPosition.Y });

        string spaces(csbi.dwSize.X, ' ');

        WriteConsole(console, spaces.data(), (DWORD)spaces.length(), &written, nullptr);

        SetConsoleCursorPosition(console, { 0, csbi.dwCursorPosition.Y });
    }
#else
    fmt::print("\33[2K");
    fflush(stdout);
#endif
}

static bool string_eq_ci(const string_view& s1, const string_view& s2) {
    if (s1.length() != s2.length())
        return false;

    auto c1 = &s1[0];
    auto c2 = &s2[0];

    for (size_t i = 0; i < s1.length(); i++) {
        auto c1a = *c1;
        auto c2a = *c2;

        if (c1a >= 'A' && c1a <= 'Z')
            c1a = c1a - 'A' + 'a';

        if (c2a >= 'A' && c2a <= 'Z')
            c2a = c2a - 'A' + 'a';

        if (c1a != c2a)
            return false;

        c1++;
        c2++;
    }

    return true;
}

static void add_inode(root& r, uint64_t inode, uint64_t ntfs_inode, bool& is_dir, runs_t& runs,
                      ntfs_file& secure, ntfs& dev, const list<uint64_t>& skiplist, enum btrfs_compression opt_compression) {
    INODE_ITEM ii;
    uint64_t file_size = 0;
    list<mapping> mappings;
    wstring_convert<codecvt_utf8_utf16<char16_t>, char16_t> convert;
    vector<tuple<uint64_t, string>> links;
    string standard_info, inline_data, sd, reparse_point, symlink;
    uint32_t atts;
    bool atts_set = false;
    map<string, tuple<uint32_t, string>> xattrs;
    string filename, wof_compressed_data;
    uint32_t cluster_size = dev.boot_sector->BytesPerSector * dev.boot_sector->SectorsPerCluster;
    bool processed_data = false;
    uint16_t compression_unit = 0;
    uint64_t vdl;
    vector<string> warnings;

    static const uint32_t sector_size = 0x1000; // FIXME

    ntfs_file f(dev, ntfs_inode);

    if (f.file_record->BaseFileRecordSegment.SegmentNumber != 0)
        return;

    is_dir = f.is_directory();

    f.loop_through_atts([&](const ATTRIBUTE_RECORD_HEADER* att, const string_view& res_data, const u16string_view& name) -> bool {
        switch (att->TypeCode) {
            case ntfs_attribute::STANDARD_INFORMATION:
                if (att->FormCode == NTFS_ATTRIBUTE_FORM::NONRESIDENT_FORM)
                    throw formatted_error("Error - STANDARD_INFORMATION is non-resident"); // FIXME - can this happen?

                standard_info = res_data;
            break;

            case ntfs_attribute::DATA:
                if (name.empty()) { // main file data
                    if (att->Flags & ATTRIBUTE_FLAG_ENCRYPTED) {
                        clear_line();

                        if (filename.empty())
                            filename = f.get_filename();

                        warnings.emplace_back(fmt::format("Skipping encrypted inode {:x} ({})", inode - inode_offset, filename));
                        return true;
                    }

                    if (att->FormCode == NTFS_ATTRIBUTE_FORM::RESIDENT_FORM && !processed_data) {
                        file_size = vdl = att->Form.Resident.ValueLength;

                        inline_data = res_data;
                    } else {
                        if (!processed_data) {
                            file_size = att->Form.Nonresident.FileSize;
                            compression_unit = att->Form.Nonresident.CompressionUnit;
                            vdl = att->Form.Nonresident.ValidDataLength;

                            if (!(att->Flags & ATTRIBUTE_FLAG_COMPRESSION_MASK))
                                compression_unit = 0;
                        }

                        list<mapping> mappings2;
                        uint64_t last_vcn;

                        if (mappings.empty())
                            last_vcn = 0;
                        else
                            last_vcn = mappings.back().vcn + mappings.back().length;

                        if (last_vcn < att->Form.Nonresident.LowestVcn)
                            mappings.emplace_back(0, last_vcn, att->Form.Nonresident.LowestVcn - last_vcn);

                        read_nonresident_mappings(att, mappings2, cluster_size, vdl);

                        mappings.splice(mappings.end(), mappings2);
                    }

                    processed_data = true;
                } else { // ADS
                    static const char xattr_prefix[] = "user.";

                    auto ads_name = convert.to_bytes(name.data(), name.data() + name.length());
                    auto max_xattr_size = (uint32_t)(tree_size - sizeof(tree_header) - sizeof(leaf_node) - offsetof(DIR_ITEM, name[0]) - ads_name.length() - (sizeof(xattr_prefix) - 1));

                    // FIXME - check xattr_name not reserved

                    if (att->Flags & ATTRIBUTE_FLAG_ENCRYPTED) {
                        clear_line();

                        if (filename.empty())
                            filename = f.get_filename();

                        warnings.emplace_back(fmt::format("Skipping encrypted ADS {}:{}", filename, ads_name));

                        break;
                    }

                    if (att->Flags & ATTRIBUTE_FLAG_COMPRESSION_MASK) {
                        clear_line();

                        if (filename.empty())
                            filename = f.get_filename();

                        warnings.emplace_back(fmt::format("Skipping compressed ADS {}:{}", filename, ads_name)); // FIXME

                        break;
                    }

                    auto name2 = xattr_prefix + ads_name;

                    uint32_t hash = calc_crc32c(0xfffffffe, (const uint8_t*)name2.data(), (uint32_t)name2.length());

                    if (att->FormCode == NTFS_ATTRIBUTE_FORM::RESIDENT_FORM) {
                        if (ads_name == "WofCompressedData")
                            wof_compressed_data = res_data;
                        else {
                            if (att->Form.Resident.ValueLength > max_xattr_size) {
                                clear_line();

                                if (filename.empty())
                                    filename = f.get_filename();

                                warnings.emplace_back(fmt::format("Skipping overly large ADS {}:{} ({} > {})", filename, ads_name, att->Form.Resident.ValueLength, max_xattr_size));

                                break;
                            }

                            xattrs.emplace(name2, make_pair(hash, res_data));
                        }
                    } else {
                        if (att->Form.Nonresident.FileSize > max_xattr_size && ads_name != "WofCompressedData") {
                            clear_line();

                            if (filename.empty())
                                filename = f.get_filename();

                            warnings.emplace_back(fmt::format("Skipping overly large ADS {}:{} ({} > {})", filename, ads_name, att->Form.Nonresident.FileSize, max_xattr_size));

                            break;
                        }

                        list<mapping> ads_mappings;
                        string ads_data;

                        read_nonresident_mappings(att, ads_mappings, cluster_size, att->Form.Nonresident.ValidDataLength);

                        ads_data.resize((size_t)sector_align(att->Form.Nonresident.FileSize, cluster_size));
                        memset(ads_data.data(), 0, ads_data.length());

                        for (const auto& m : ads_mappings) {
                            dev.seek(m.lcn * cluster_size);
                            dev.read(ads_data.data() + (m.vcn * cluster_size), (size_t)(m.length * cluster_size));
                        }

                        ads_data.resize((size_t)att->Form.Nonresident.FileSize);

                        if (ads_name == "WofCompressedData")
                            wof_compressed_data = ads_data;
                        else
                            xattrs.emplace(name2, make_pair(hash, ads_data));
                    }
                }
            break;

            case ntfs_attribute::FILE_NAME: {
                if (att->FormCode == NTFS_ATTRIBUTE_FORM::NONRESIDENT_FORM)
                    throw formatted_error("Error - FILE_NAME is non-resident"); // FIXME - can this happen?

                if (att->Form.Resident.ValueLength < offsetof(FILE_NAME, FileName[0]))
                    throw formatted_error("FILE_NAME was truncated");

                auto fn = reinterpret_cast<const FILE_NAME*>(res_data.data());

                if (fn->Namespace != FILE_NAME_DOS) {
                    if (att->Form.Resident.ValueLength < offsetof(FILE_NAME, FileName[0]) + (fn->FileNameLength * sizeof(char16_t)))
                        throw formatted_error("FILE_NAME was truncated");

                    auto name2 = convert.to_bytes(fn->FileName, fn->FileName + fn->FileNameLength);

                    uint64_t parent = fn->Parent.SegmentNumber;

                    if (!is_dir || links.empty()) {
                        bool skip = false;

                        for (auto n : skiplist) {
                            if (n == parent) {
                                skip = true;
                                break;
                            }
                        }

                        if (!skip) {
                            for (const auto& l : links) {
                                if (get<0>(l) == parent && get<1>(l) == name2) {
                                    skip = true;
                                    break;
                                }
                            }
                        }

                        if (!skip)
                            links.emplace_back(parent, name2);
                    }
                }

                break;
            }

            case ntfs_attribute::SYMBOLIC_LINK:
                if (att->FormCode == NTFS_ATTRIBUTE_FORM::NONRESIDENT_FORM)
                    throw formatted_error("Error - SYMBOLIC_LINK is non-resident"); // FIXME - can this happen?

                reparse_point = res_data;
                symlink.clear();

                if (!is_dir && reparse_point.size() > offsetof(REPARSE_DATA_BUFFER, SymbolicLinkReparseBuffer.PathBuffer)) {
                    auto rpb = reinterpret_cast<const REPARSE_DATA_BUFFER*>(reparse_point.data());

                    if ((rpb->ReparseTag == IO_REPARSE_TAG_SYMLINK && rpb->SymbolicLinkReparseBuffer.Flags & SYMLINK_FLAG_RELATIVE) ||
                        rpb->ReparseTag == IO_REPARSE_TAG_LX_SYMLINK) {

                        if (reparse_point.size() < offsetof(REPARSE_DATA_BUFFER, SymbolicLinkReparseBuffer.PathBuffer) +
                                                   rpb->SymbolicLinkReparseBuffer.PrintNameOffset +
                                                   rpb->SymbolicLinkReparseBuffer.PrintNameLength) {
                            clear_line();

                            if (filename.empty())
                                filename = f.get_filename();

                            warnings.emplace_back(fmt::format("Reparse point buffer of {} was truncated.", filename));
                        } else {
                            symlink = convert.to_bytes(&rpb->SymbolicLinkReparseBuffer.PathBuffer[rpb->SymbolicLinkReparseBuffer.PrintNameOffset / sizeof(char16_t)],
                                                       &rpb->SymbolicLinkReparseBuffer.PathBuffer[(rpb->SymbolicLinkReparseBuffer.PrintNameOffset + rpb->SymbolicLinkReparseBuffer.PrintNameLength) / sizeof(char16_t)]);

                            for (auto& c : symlink) {
                                if (c == '\\')
                                    c = '/';
                            }

                            reparse_point = "";
                        }
                    }
                }
            break;

            case ntfs_attribute::SECURITY_DESCRIPTOR: {
                auto max_sd_size = (uint32_t)(tree_size - sizeof(tree_header) - sizeof(leaf_node) - offsetof(DIR_ITEM, name[0]) - sizeof(EA_NTACL) + 1);

                if (att->FormCode == NTFS_ATTRIBUTE_FORM::RESIDENT_FORM) {
                    if (att->Form.Resident.ValueLength > max_sd_size) {
                        clear_line();

                        if (filename.empty())
                            filename = f.get_filename();

                        warnings.emplace_back(fmt::format("Skipping overly large SD for {} ({} > {})", filename, att->Form.Resident.ValueLength, max_sd_size));

                        break;
                    }

                    sd = res_data;
                } else {
                    if (att->Form.Nonresident.FileSize > max_sd_size) {
                        clear_line();

                        if (filename.empty())
                            filename = f.get_filename();

                        warnings.emplace_back(fmt::format("Skipping overly large SD for {} ({} > {})", filename, att->Form.Nonresident.FileSize, max_sd_size));

                        break;
                    }

                    list<mapping> sd_mappings;

                    read_nonresident_mappings(att, sd_mappings, cluster_size, att->Form.Nonresident.ValidDataLength);

                    sd.resize((size_t)sector_align(att->Form.Nonresident.FileSize, cluster_size));
                    memset(sd.data(), 0, sd.length());

                    for (const auto& m : sd_mappings) {
                        dev.seek(m.lcn * cluster_size);
                        dev.read(sd.data() + (m.vcn * cluster_size), (size_t)(m.length * cluster_size));
                    }

                    sd.resize((size_t)att->Form.Nonresident.FileSize);
                }

                break;
            }

            default:
            break;
        }

        return true;
    });

    // skip page files
    if (links.size() == 1 && get<0>(links.front()) == NTFS_ROOT_DIR_INODE) {
        if (string_eq_ci(get<1>(links.front()), "pagefile.sys") || string_eq_ci(get<1>(links.front()), "hiberfil.sys") ||
            string_eq_ci(get<1>(links.front()), "swapfile.sys"))
            return;
    }

    if (links.empty())
        return; // don't create orphaned inodes

    if (compression_unit != 0) {
        string compdata;
        uint64_t cus = 1 << compression_unit;

        compdata.resize((size_t)(cus * cluster_size));

        try {
            while (!mappings.empty()) {
                uint64_t clusters = 0, compsize;
                bool compressed = false;

                while (clusters < cus) {
                    if (mappings.empty()) {
                        compressed = true;
                        memset(compdata.data() + (clusters * cluster_size), 0, (size_t)((cus - clusters) * cluster_size));
                        break;
                    }

                    auto& m = mappings.front();
                    auto l = min(m.length, cus - clusters);

                    if (m.lcn == 0) {
                        memset(compdata.data() + (clusters * cluster_size), 0, (size_t)(l * cluster_size));

                        if (l < m.length) {
                            m.vcn += l;
                            m.length -= l;
                        } else
                            mappings.pop_front();

                        compressed = true;
                    } else {
                        dev.seek(m.lcn * cluster_size);
                        dev.read(compdata.data() + (clusters * cluster_size), (size_t)(l * cluster_size));

                        if (l < m.length) {
                            m.lcn += l;
                            m.vcn += l;
                            m.length -= l;
                        } else
                            mappings.pop_front();
                    }

                    clusters += l;
                }

                if (!compressed) {
                    if (filename.empty())
                        filename = f.get_filename();

                    inline_data += compdata;
                } else {
                    compsize = compdata.length();

                    if (file_size - inline_data.length() < compsize)
                        compsize = file_size - inline_data.length();

                    inline_data += lznt1_decompress(compdata, (uint32_t)compsize);
                }

                if (inline_data.length() >= file_size) {
                    inline_data.resize((size_t)file_size);
                    break;
                }
            }
        } catch (const exception& e) {
            if (filename.empty())
                filename = f.get_filename();

            throw formatted_error("{}: {}", filename, e.what());
        }
    }

    for (const auto& w : warnings) {
        fmt::print(stderr, "{}\n", w);
    }

    memset(&ii, 0, sizeof(INODE_ITEM));

    if (standard_info.length() >= offsetof(STANDARD_INFORMATION, MaximumVersions)) {
        auto si = reinterpret_cast<const STANDARD_INFORMATION*>(standard_info.data());
        uint32_t defda = 0;

        atts = si->FileAttributes;

        if (links.size() == 1 && get<1>(links[0])[0] == '.')
            defda |= FILE_ATTRIBUTE_HIDDEN;

        if (is_dir) {
            defda |= FILE_ATTRIBUTE_DIRECTORY;
            atts |= FILE_ATTRIBUTE_DIRECTORY;
        } else {
            defda |= FILE_ATTRIBUTE_ARCHIVE;
            atts &= ~FILE_ATTRIBUTE_DIRECTORY;
        }

        if (!reparse_point.empty() || !symlink.empty())
            atts |= FILE_ATTRIBUTE_REPARSE_POINT;
        else
            atts &= ~FILE_ATTRIBUTE_REPARSE_POINT;

        if (atts != defda)
            atts_set = true;
    }

    if (standard_info.length() >= offsetof(STANDARD_INFORMATION, OwnerId)) {
        auto si = reinterpret_cast<const STANDARD_INFORMATION*>(standard_info.data());

        ii.otime = win_time_to_unix(si->CreationTime);
        ii.st_atime = win_time_to_unix(si->LastAccessTime);
        ii.st_mtime = win_time_to_unix(si->LastWriteTime);
        ii.st_ctime = win_time_to_unix(si->ChangeTime);
    }

    if (sd.empty() && standard_info.length() >= offsetof(STANDARD_INFORMATION, QuotaCharged)) {
        auto si = reinterpret_cast<const STANDARD_INFORMATION*>(standard_info.data());

        sd = find_sd(si->SecurityId, secure, dev);

        if (sd.empty()) {
            clear_line();

            if (filename.empty())
                filename = f.get_filename();

            fmt::print(stderr, "Could not find SecurityId {} ({})\n", si->SecurityId, filename);
        }
    }

    if (reparse_point.length() > sizeof(uint32_t) && *(uint32_t*)reparse_point.data() == IO_REPARSE_TAG_WOF) {
        try {
            if (reparse_point.length() < offsetof(reparse_point_header, DataBuffer)) {
                throw formatted_error("IO_REPARSE_TAG_WOF reparse point buffer was {} bytes, expected at least {}.",
                                      reparse_point.length(), offsetof(reparse_point_header, DataBuffer));
            }

            auto rph = (reparse_point_header*)reparse_point.data();

            if (reparse_point.length() < offsetof(reparse_point_header, DataBuffer) + rph->ReparseDataLength) {
                throw formatted_error("IO_REPARSE_TAG_WOF reparse point buffer was {} bytes, expected {}.",
                                      reparse_point.length(), offsetof(reparse_point_header, DataBuffer) + rph->ReparseDataLength);
            }

            if (rph->ReparseDataLength < sizeof(wof_external_info)) {
                throw formatted_error("rph->ReparseDataLength was {} bytes, expected at least {}.",
                                      rph->ReparseDataLength, sizeof(wof_external_info));
            }

            auto wofei = (wof_external_info*)rph->DataBuffer;

            if (wofei->Version != WOF_CURRENT_VERSION)
                throw formatted_error("Unsupported WOF version {}.", wofei->Version);

            if (wofei->Provider == WOF_PROVIDER_WIM)
                throw formatted_error("Unsupported WOF provider WOF_PROVIDER_WIM.");
            else if (wofei->Provider != WOF_PROVIDER_FILE)
                throw formatted_error("Unsupported WOF provider {}.", wofei->Provider);

            if (rph->ReparseDataLength < sizeof(wof_external_info) + sizeof(file_provider_external_info_v0)) {
                throw formatted_error("rph->ReparseDataLength was {} bytes, expected {}.",
                                      rph->ReparseDataLength, sizeof(wof_external_info) + sizeof(file_provider_external_info_v0));
            }

            auto fpei = *(file_provider_external_info_v0*)&wofei[1];

            if (fpei.Version != FILE_PROVIDER_CURRENT_VERSION) {
                throw formatted_error("rph->FILE_PROVIDER_EXTERNAL_INFO_V0 Version was {}, expected {}.",
                                      fpei.Version, FILE_PROVIDER_CURRENT_VERSION);
            }

            reparse_point.clear();
            mappings.clear();

            switch (fpei.Algorithm) {
                case FILE_PROVIDER_COMPRESSION_XPRESS4K:
                    inline_data = do_xpress_decompress(wof_compressed_data, (uint32_t)file_size, 4096);
                    break;

                case FILE_PROVIDER_COMPRESSION_LZX:
                    inline_data = do_lzx_decompress(wof_compressed_data, (uint32_t)file_size);
                    break;

                case FILE_PROVIDER_COMPRESSION_XPRESS8K:
                    inline_data = do_xpress_decompress(wof_compressed_data, (uint32_t)file_size, 8192);
                    break;

                case FILE_PROVIDER_COMPRESSION_XPRESS16K:
                    inline_data = do_xpress_decompress(wof_compressed_data, (uint32_t)file_size, 16384);
                    break;

                default:
                    throw formatted_error("Unrecognized WOF compression algorithm {}", fpei.Algorithm);
            }
        } catch (const exception& e) {
            if (filename.empty())
                filename = f.get_filename();

            fmt::print(stderr, "{}: {}\n", filename, e.what());
        }
    }

    ii.generation = 1;
    ii.transid = 1;

    if (!is_dir && !reparse_point.empty()) {
        inline_data = reparse_point;
        file_size = reparse_point.size();
    } else if (!symlink.empty()) {
        mappings.clear();
        inline_data = symlink;
        file_size = symlink.size();
    }

    if (!is_dir)
        ii.st_size = file_size;

    ii.st_nlink = (uint32_t)links.size();

    if (is_dir)
        ii.st_mode = __S_IFDIR | S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
    else
        ii.st_mode = __S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

    if (!symlink.empty())
        ii.st_mode |= __S_IFLNK;

    ii.sequence = 1;

    // FIXME - xattrs (EAs, etc.)
    // FIXME - LXSS

    if (!mappings.empty()) {
        size_t extlen = offsetof(EXTENT_DATA, data[0]) + sizeof(EXTENT_DATA2);
        auto ed = (EXTENT_DATA*)malloc(extlen);
        if (!ed)
            throw bad_alloc();

        mapped_inodes++;

        auto ed2 = (EXTENT_DATA2*)&ed->data;

        ed->generation = 1;
        ed->compression = btrfs_compression::none;
        ed->encryption = 0;
        ed->encoding = 0;
        ed->type = btrfs_extent_type::regular;

        try {
            process_mappings(dev, inode, mappings, runs);

            if (vdl < file_size) {
                uint64_t alloc_size = sector_align(file_size, sector_size);
                uint64_t alloc_vdl = sector_align(vdl, sector_size);

                if (!mappings.empty() && (mappings.back().vcn + mappings.back().length) < alloc_size / sector_size) {
                    mappings.emplace_back(0, mappings.back().vcn + mappings.back().length,
                                          (alloc_size / sector_size) - mappings.back().vcn - mappings.back().length);
                }

                while (alloc_vdl < alloc_size) { // for whole sectors, replace with sparse extents
                    if (!mappings.empty()) {
                        auto& m = mappings.back();

                        if (m.length * sector_size > alloc_size - alloc_vdl) {
                            uint64_t sub = (alloc_size - alloc_vdl) / sector_size;

                            if (sub > 0) {
                                m.length -= sub * sector_size;
                                alloc_size -= sub * sector_size;
                            }

                            break;
                        } else {
                            alloc_size -= m.length * sector_size;
                            mappings.pop_back();
                        }
                    } else {
                        alloc_size = alloc_vdl;
                        break;
                    }
                }

                if (vdl < alloc_size) { // zero end of final sector if necessary
                    string sector;

                    sector.resize(sector_size);

                    dev.seek((mappings.back().lcn + mappings.back().length - 1) * cluster_size);
                    dev.read(sector.data(), sector.length());

                    memset(sector.data() + (vdl % sector_size), 0, sector_size - (vdl % sector_size));

                    dev.seek((mappings.back().lcn + mappings.back().length - 1) * cluster_size);
                    dev.write(sector.data(), sector.length());
                }
            }

            for (const auto& m : mappings) {
                if (m.lcn != 0) { // not sparse
                    ed->decoded_size = ed2->size = ed2->num_bytes = m.length * dev.boot_sector->BytesPerSector * dev.boot_sector->SectorsPerCluster;
                    ii.st_blocks += ed->decoded_size;

                    ed2->address = (m.lcn * dev.boot_sector->BytesPerSector * dev.boot_sector->SectorsPerCluster) + chunk_virt_offset;
                    ed2->offset = 0;

                    add_item(r, inode, TYPE_EXTENT_DATA, m.vcn * dev.boot_sector->BytesPerSector * dev.boot_sector->SectorsPerCluster, ed, (uint16_t)extlen);
                }
            }
        } catch (...) {
            free(ed);
            throw;
        }

        free(ed);
    } else if (!inline_data.empty()) {
        if (inline_data.length() > max_inline) {
            vector<uint8_t> buf(offsetof(EXTENT_DATA, data[0]) + sizeof(EXTENT_DATA2));
            auto compression = opt_compression;

            auto& ed = *(EXTENT_DATA*)buf.data();
            auto& ed2 = *(EXTENT_DATA2*)&ed.data;

            rewritten_inodes++;

            ed.generation = 1;
            ed.compression = btrfs_compression::none;
            ed.encryption = 0;
            ed.encoding = 0;
            ed.type = btrfs_extent_type::regular;

            // round to nearest sector, and zero end

            if (inline_data.length() & (sector_size - 1)) {
                auto oldlen = inline_data.length();

                inline_data.resize((size_t)sector_align(inline_data.length(), sector_size));
                memset(inline_data.data() + oldlen, 0, inline_data.length() - oldlen);
            }

            // FIXME - do by sparse extents, if longer than a sector
            if (vdl < inline_data.length())
                memset(inline_data.data() + vdl, 0, (size_t)(inline_data.length() - vdl));

            uint64_t pos = 0;
            string_view data = inline_data;

            while (!data.empty()) {
                uint64_t len, lcn, cl;
                bool inserted = false;
                string compdata;

                if (compression == btrfs_compression::none)
                    len = min(max_extent_size, data.length());
#if defined(WITH_ZLIB) || defined(WITH_LZO) || defined(WITH_ZSTD)
                else if (data.length() <= cluster_size) {
                    len = min(max_extent_size, data.length());
                    ed.compression = btrfs_compression::none;
                } else {
                    optional<string> c;

                    len = min(max_comp_extent_size, data.length());

                    switch (compression) {
#ifdef WITH_ZLIB
                        case btrfs_compression::zlib:
                            c = zlib_compress(data.substr(0, len), cluster_size);
                            break;
#endif

#ifdef WITH_LZO
                        case btrfs_compression::lzo:
                            c = lzo_compress(data.substr(0, len), cluster_size);
                            break;
#endif

#ifdef WITH_ZSTD
                        case btrfs_compression::zstd:
                            c = zstd_compress(data.substr(0, len), cluster_size);
                            break;
#endif
                        default:
                            break;
                    }

                    if (c.has_value()) {
                        compdata = c.value();
                        ed.compression = compression;

                        ii.flags |= BTRFS_INODE_COMPRESS;
                    } else // incompressible
                        ed.compression = btrfs_compression::none;

                    // if first part of file incompressible, give up on rest and add nocomp flag
                    if (pos == 0 && ed.compression == btrfs_compression::none) {
                        ii.flags |= BTRFS_INODE_NOCOMPRESS;
                        compression = btrfs_compression::none;
                        len = min(max_extent_size, data.length());
                    }

                    // FIXME - set xattr for compression type?
                }
#endif

                ed.decoded_size = ed2.num_bytes = len;
                ed2.size = ed.compression == btrfs_compression::none ? len : compdata.length();
                ii.st_blocks += ed.decoded_size;

                ed2.address = allocate_data(ed2.size, true);
                ed2.offset = 0;

                dev.seek(ed2.address - chunk_virt_offset);

                if (ed.compression == btrfs_compression::none)
                    dev.write(data.data(), (size_t)len);
                else
                    dev.write(compdata.data(), compdata.length());

                add_item(r, inode, TYPE_EXTENT_DATA, pos, &ed, (uint16_t)buf.size());

                lcn = (ed2.address - chunk_virt_offset) / cluster_size;
                cl = ed2.size / cluster_size;

                auto& rl = runs[(ed2.address - chunk_virt_offset) / data_chunk_size];

                for (auto it = rl.begin(); it != rl.end(); it++) {
                    auto& r = *it;

                    if (r.offset >= lcn + cl) {
                        rl.emplace(it, lcn, cl, inode, pos / cluster_size, false, true);
                        inserted = true;
                        break;
                    }
                }

                if (!inserted)
                    rl.emplace_back(lcn, cl, inode, pos / cluster_size, false, true);

                if (data.length() > len) {
                    pos += len;
                    data = data.substr((size_t)len);
                } else
                    break;
            }

            inline_data.clear();
        } else {
            size_t extlen = offsetof(EXTENT_DATA, data[0]) + inline_data.length();
            auto ed = (EXTENT_DATA*)malloc(extlen);
            if (!ed)
                throw bad_alloc();

            inline_inodes++;

            // FIXME - compress inline extents?

            ed->generation = 1;
            ed->decoded_size = inline_data.length();
            ed->compression = btrfs_compression::none;
            ed->encryption = 0;
            ed->encoding = 0;
            ed->type = btrfs_extent_type::inline_extent;

            memcpy(ed->data, inline_data.data(), inline_data.length());

            if (vdl < inline_data.length())
                memset(ed->data + vdl, 0, (size_t)(inline_data.length() - vdl));

            add_item(r, inode, TYPE_EXTENT_DATA, 0, ed, (uint16_t)extlen);

            free(ed);

            ii.st_blocks = inline_data.length();
        }
    }

    add_item(r, inode, TYPE_INODE_ITEM, 0, &ii, sizeof(INODE_ITEM));

    {
        uint8_t type;

        if (is_dir)
            type = BTRFS_TYPE_DIRECTORY;
        else if (!symlink.empty())
            type = BTRFS_TYPE_SYMLINK;
        else
            type = BTRFS_TYPE_FILE;

        for (const auto& l : links) {
            if (get<0>(l) == NTFS_ROOT_DIR_INODE)
                link_inode(r, inode, SUBVOL_ROOT_INODE, get<1>(l), type);
            else
                link_inode(r, inode, get<0>(l) + inode_offset, get<1>(l), type);
        }
    }

    if (!sd.empty()) {
        // FIXME - omit SD if only one hard link and implied from parent?
        xattrs.emplace(EA_NTACL, make_pair(EA_NTACL_HASH, sd));
    }

    if (atts_set) {
        char val[16], *val2;

        val2 = &val[sizeof(val) - 1];

        do {
            uint8_t c = atts % 16;
            *val2 = (char)(c <= 9 ? (c + '0') : (c - 0xa + 'a'));

            val2--;
            atts >>= 4;
        } while (atts != 0);

        *val2 = 'x';
        val2--;
        *val2 = '0';

        xattrs.emplace(EA_DOSATTRIB, make_pair(EA_DOSATTRIB_HASH, string_view(val2, val + sizeof(val) - val2)));
    }

    if (!reparse_point.empty() && is_dir)
        xattrs.emplace(EA_REPARSE, make_pair(EA_REPARSE_HASH, reparse_point));

    for (const auto& xa : xattrs) {
        // FIXME - collisions (make hash key of map?)
        set_xattr(r, inode, xa.first, get<0>(xa.second), get<1>(xa.second));
    }
}

static void create_inodes(root& r, const string& mftbmp, ntfs& dev, runs_t& runs, ntfs_file& secure,
                          enum btrfs_compression compression) {
    list<space> inodes;
    list<uint64_t> skiplist;
    uint64_t total = 0, num = 0;

    r.dir_seqs[SUBVOL_ROOT_INODE] = 3;

    parse_bitmap(mftbmp, inodes);

    for (const auto& l : inodes) {
        total += l.length;
    }

    while (!inodes.empty()) {
        auto& run = inodes.front();
        uint64_t ntfs_inode = run.offset;
        uint64_t inode = ntfs_inode + inode_offset;
        bool dir;

        try {
            if (ntfs_inode >= first_ntfs_inode)
                add_inode(r, inode, ntfs_inode, dir, runs, secure, dev, skiplist, compression);
            else if (ntfs_inode != NTFS_ROOT_DIR_INODE)
                populate_skip_list(dev, ntfs_inode, skiplist);
        } catch (...) {
            clear_line();
            throw;
        }

        num++;
        fmt::print("Processing inode {} / {} ({:1.1f}%)\r", num, total, (float)num * 100.0f / (float)total);
        fflush(stdout);

        if (run.length == 1)
            inodes.pop_front();
        else {
            run.offset++;
            run.length--;
        }
    }

    fmt::print("\n");
}

static void create_data_extent_items(root& extent_root, const runs_t& runs, uint32_t cluster_size, uint64_t image_subvol_id,
                                     uint64_t image_inode) {
    for (const auto& rs : runs) {
        for (const auto& r : rs.second) {
            uint64_t img_addr;

            if (r.inode == dummy_inode)
                continue;

            if (r.relocated) {
                for (const auto& reloc : relocs) {
                    if (reloc.new_start == r.offset) {
                        img_addr = reloc.old_start * cluster_size;
                        break;
                    }
                }
            } else
                img_addr = r.offset * cluster_size;

            if (r.inode == 0) {
                data_item di;

                di.extent_item.refcount = 1;
                di.extent_item.generation = 1;
                di.extent_item.flags = EXTENT_ITEM_DATA;
                di.type = TYPE_EXTENT_DATA_REF;
                di.edr.root = image_subvol_id;
                di.edr.objid = image_inode;
                di.edr.count = 1;
                di.edr.offset = img_addr;

                add_item(extent_root, (r.offset * cluster_size) + chunk_virt_offset, TYPE_EXTENT_ITEM, r.length * cluster_size,
                         &di, sizeof(data_item));
            } else if (r.not_in_img) {
                data_item di;

                di.extent_item.refcount = 1;
                di.extent_item.generation = 1;
                di.extent_item.flags = EXTENT_ITEM_DATA;
                di.type = TYPE_EXTENT_DATA_REF;
                di.edr.root = BTRFS_ROOT_FSTREE;
                di.edr.objid = r.inode;
                di.edr.count = 1;
                di.edr.offset = r.file_offset * cluster_size;

                add_item(extent_root, (r.offset * cluster_size) + chunk_virt_offset, TYPE_EXTENT_ITEM, r.length * cluster_size,
                         &di, sizeof(data_item));
            } else {
                data_item2 di2;

                di2.extent_item.refcount = 2;
                di2.extent_item.generation = 1;
                di2.extent_item.flags = EXTENT_ITEM_DATA;
                di2.type1 = TYPE_EXTENT_DATA_REF;
                di2.edr1.root = image_subvol_id;
                di2.edr1.objid = image_inode;
                di2.edr1.count = 1;
                di2.edr1.offset = img_addr;
                di2.type2 = TYPE_EXTENT_DATA_REF;
                di2.edr2.root = BTRFS_ROOT_FSTREE;
                di2.edr2.objid = r.inode;
                di2.edr2.count = 1;
                di2.edr2.offset = r.file_offset * cluster_size;

                add_item(extent_root, (r.offset * cluster_size) + chunk_virt_offset, TYPE_EXTENT_ITEM, r.length * cluster_size,
                         &di2, sizeof(data_item2));
            }
        }
    }
}

static void calc_checksums(root& csum_root, runs_t runs, ntfs& dev, enum btrfs_csum_type csum_type) {
    uint32_t sector_size = 0x1000; // FIXME
    uint32_t cluster_size = dev.boot_sector->BytesPerSector * dev.boot_sector->SectorsPerCluster;
    list<space> runs2;
    uint64_t total = 0, num = 0;
    uint32_t csum_size;

    switch (csum_type) {
        case btrfs_csum_type::crc32c:
            csum_size = sizeof(uint32_t);
            break;

        case btrfs_csum_type::xxhash:
            csum_size = sizeof(uint64_t);
            break;

        case btrfs_csum_type::sha256:
        case btrfs_csum_type::blake2:
            csum_size = 32;
            break;
    }

    // See __MAX_CSUM_ITEMS in kernel

    auto max_run = (uint32_t)((tree_size - sizeof(tree_header) - (sizeof(leaf_node) * 2)) / csum_size) - 1;

    // FIXME - these are clusters, when they should be sectors

    // split and merge runs

    for (auto& r : runs) {
        auto& rs = r.second;
        bool first = true;

        while (!rs.empty()) {
            auto& r = rs.front();

            if (r.inode == dummy_inode) {
                rs.pop_front();
                continue;
            }

            if (first || runs2.back().offset + runs2.back().length < r.offset || runs2.back().length == max_run) {
                // create new run

                if (r.length > max_run) {
                    runs2.emplace_back(r.offset, max_run);
                    r.offset += max_run;
                    r.length -= max_run;
                } else {
                    runs2.emplace_back(r.offset, r.length);
                    rs.pop_front();
                }

                first = false;

                continue;
            }

            // continue existing run

            if (runs2.back().length + r.length <= max_run) {
                runs2.back().length += r.length;
                rs.pop_front();
                continue;
            }

            r.offset += max_run - runs2.back().length;
            r.length -= max_run - runs2.back().length;
            runs2.back().length = max_run;
        }
    }

    for (const auto& r : runs2) {
        total += r.length;
    }

    for (const auto& r : runs2) {
        string data;
        vector<uint8_t> csums;

        if (r.offset * cluster_size >= orig_device_size)
            break;

        data.resize((size_t)(r.length * cluster_size));
        csums.resize((size_t)(r.length * cluster_size * csum_size / sector_size));

        dev.seek(r.offset * cluster_size);
        dev.read(data.data(), data.length());

        string_view sv = data;

        auto msg = [&]() {
            num++;

            if (num % 1000 == 0 || num == total) {
                fmt::print("Calculating checksums {} / {} ({:1.1f}%)\r", num, total, (float)num * 100.0f / (float)total);
                fflush(stdout);
            }
        };

        switch (csum_type) {
            case btrfs_csum_type::crc32c: {
                auto csum = (uint32_t*)&csums[0];

                while (sv.length() > 0) {
                    *csum = ~calc_crc32c(0xffffffff, (const uint8_t*)sv.data(), sector_size);

                    csum++;
                    sv = sv.substr(sector_size);

                    msg();
                }

                break;
            }

            case btrfs_csum_type::xxhash: {
                auto csum = (uint64_t*)&csums[0];

                while (sv.length() > 0) {
                    *csum = XXH64(sv.data(), sector_size, 0);

                    csum++;
                    sv = sv.substr(sector_size);

                    msg();
                }

                break;
            }

            case btrfs_csum_type::sha256: {
                auto csum = (uint8_t*)&csums[0];

                while (sv.length() > 0) {
                    calc_sha256(csum, sv.data(), sector_size);

                    csum += csum_size;
                    sv = sv.substr(sector_size);

                    msg();
                }

                break;
            }

            case btrfs_csum_type::blake2: {
                auto csum = (uint8_t*)&csums[0];

                while (sv.length() > 0) {
                    blake2b(csum, csum_size, sv.data(), sector_size);

                    csum += csum_size;
                    sv = sv.substr(sector_size);

                    msg();
                }

                break;
            }
        }

        add_item(csum_root, EXTENT_CSUM_ID, TYPE_EXTENT_CSUM, (r.offset * cluster_size) + chunk_virt_offset, &csums[0], (uint16_t)(r.length * cluster_size * csum_size / sector_size));
    }

    fmt::print("\n");
}

static void protect_cluster(ntfs& dev, runs_t& runs, uint64_t cluster) {
    if (!split_runs(dev, runs, cluster, 1, dummy_inode, 0))
        return;

    string sb;
    uint32_t cluster_size = dev.boot_sector->BytesPerSector * dev.boot_sector->SectorsPerCluster;
    uint64_t addr = allocate_data(cluster_size, false) - chunk_virt_offset;

    if ((cluster + 1) * cluster_size > orig_device_size)
        sb.resize((size_t)(orig_device_size - (cluster * cluster_size)));
    else
        sb.resize((size_t)cluster_size);

    dev.seek(cluster * cluster_size);
    dev.read(sb.data(), sb.length());

    dev.seek(addr);
    dev.write(sb.data(), sb.length());

    relocs.emplace_back(cluster, 1, addr / cluster_size);

    uint64_t clusters_per_chunk = data_chunk_size / (uint64_t)cluster_size;
    uint64_t cluster_addr = (cluster * cluster_size) + chunk_virt_offset;

    for (auto& c : chunks) {
        if (c.offset <= cluster_addr && c.offset + c.length > cluster_addr) {
            c.used -= cluster_size;
            break;
        }
    }

    uint64_t chunk = (addr / (uint64_t)cluster_size) / clusters_per_chunk;

    if (runs.count(chunk) != 0) {
        auto& r = runs.at(chunk);

        for (auto it = r.begin(); it != r.end(); it++) {
            if (it->offset > addr / cluster_size) {
                r.emplace(it, addr / cluster_size, 1, 0, 0, true);
                return;
            }
        }
    }

    auto& r = runs[chunk];

    r.emplace_back(addr / cluster_size, 1, 0, 0, true);
}

static void protect_superblocks(ntfs& dev, runs_t& runs) {
    uint32_t cluster_size = dev.boot_sector->BytesPerSector * dev.boot_sector->SectorsPerCluster;

    unsigned int i = 0;
    while (superblock_addrs[i] != 0) {
        if (superblock_addrs[i] > device_size - sizeof(superblock))
            break;

        uint64_t cluster_start = (superblock_addrs[i] - (superblock_addrs[i] % stripe_length)) / cluster_size;
        uint64_t cluster_end = sector_align(superblock_addrs[i] - (superblock_addrs[i] % stripe_length) + stripe_length, cluster_size) / cluster_size;

        for (uint64_t j = cluster_start; j < cluster_end; j++) {
            protect_cluster(dev, runs, j);
        }

        i++;
    }

    // also relocate first cluster

    protect_cluster(dev, runs, 0);

    if (reloc_last_sector)
        protect_cluster(dev, runs, device_size / cluster_size);
}

static void clear_first_cluster(ntfs& dev) {
    uint32_t cluster_size = dev.boot_sector->BytesPerSector * dev.boot_sector->SectorsPerCluster;
    string data;

    data.resize(cluster_size);

    memset(data.data(), 0, cluster_size);

    dev.seek(0);
    dev.write(data.data(), data.length());
}

static void calc_used_space(const runs_t& runs, uint32_t cluster_size) {
    for (const auto& rl : runs) {
        uint64_t offset = (rl.first * data_chunk_size) + chunk_virt_offset;

        for (auto& c : chunks) {
            if (offset == c.offset) {
                for (const auto& r : rl.second) {
                    c.used += r.length * cluster_size;
                }

                break;
            }
        }
    }
}

static void populate_root_root(root& root_root) {
    INODE_ITEM ii;

    static const char default_subvol[] = "default";
    static const uint32_t default_hash = 0x8dbfc2d2;

    for (const auto& r : roots) {
        if (r.id != BTRFS_ROOT_ROOT && r.id != BTRFS_ROOT_CHUNK)
            add_to_root_root(r, root_root);
    }

    add_inode_ref(root_root, BTRFS_ROOT_FSTREE, BTRFS_ROOT_TREEDIR, 0, "default");

    memset(&ii, 0, sizeof(INODE_ITEM));

    ii.generation = 1;
    ii.transid = 1;
    ii.st_nlink = 1;
    ii.st_mode = __S_IFDIR | S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;

    add_item(root_root, BTRFS_ROOT_TREEDIR, TYPE_INODE_ITEM, 0, &ii, sizeof(INODE_ITEM));

    add_inode_ref(root_root, BTRFS_ROOT_TREEDIR, BTRFS_ROOT_TREEDIR, 0, "..");

    vector<uint8_t> buf(offsetof(DIR_ITEM, name[0]) + sizeof(default_subvol) - 1);

    auto& di = *(DIR_ITEM*)buf.data();

    di.key.obj_id = BTRFS_ROOT_FSTREE;
    di.key.obj_type = TYPE_ROOT_ITEM;
    di.key.offset = 0xffffffffffffffff;
    di.transid = 0;
    di.m = 0;
    di.n = sizeof(default_subvol) - 1;
    di.type = BTRFS_TYPE_DIRECTORY;
    memcpy(di.name, default_subvol, sizeof(default_subvol) - 1);

    add_item(root_root, BTRFS_ROOT_TREEDIR, TYPE_DIR_ITEM, default_hash, &di, (uint16_t)buf.size());
}

static void add_subvol_uuid(root& r) {
    add_item(r, *(uint64_t*)&subvol_uuid, TYPE_SUBVOL_UUID, *(uint64_t*)&subvol_uuid.uuid[sizeof(uint64_t)],
             &image_subvol_id, sizeof(image_subvol_id));
}

static void update_dir_sizes(root& r) {
    for (auto& it : r.items) {
        if (it.first.obj_type == TYPE_INODE_ITEM && r.dir_size.count(it.first.obj_id) != 0) {
            auto ii = (INODE_ITEM*)it.second.data;

            // FIXME - would it speed things up if we removed the entry from dir_size map here?

            ii->st_size = r.dir_size.at(it.first.obj_id);
        }
    }
}

static void convert(ntfs& dev, enum btrfs_compression compression, enum btrfs_csum_type csum_type) {
    uint32_t sector_size = 0x1000; // FIXME
    uint64_t cluster_size = (uint64_t)dev.boot_sector->BytesPerSector * (uint64_t)dev.boot_sector->SectorsPerCluster;
    runs_t runs;

    static const uint64_t image_inode = 0x101;

    // FIXME - die if cluster size not multiple of 4096

    {
        default_random_engine generator;

        generator.seed((unsigned int)chrono::high_resolution_clock::now().time_since_epoch().count());

        fs_uuid = generate_uuid(generator);
        chunk_uuid = generate_uuid(generator);
        dev_uuid = generate_uuid(generator);
        subvol_uuid = generate_uuid(generator);
    }

    device_size = orig_device_size = dev.boot_sector->TotalSectors * dev.boot_sector->BytesPerSector;

    if (device_size % sector_size != 0) {
        device_size -= device_size % sector_size;
        reloc_last_sector = true;
    }

    space_list.emplace_back(0, device_size);

    ntfs_file bitmap(dev, NTFS_BITMAP_INODE);

    auto bmpdata = bitmap.read();

    create_data_chunks(dev, bmpdata);

    roots.emplace_back(BTRFS_ROOT_ROOT);
    root& root_root = roots.back();

    roots.emplace_back(BTRFS_ROOT_EXTENT);
    root& extent_root = roots.back();

    roots.emplace_back(BTRFS_ROOT_CHUNK);
    root& chunk_root = roots.back();

    add_dev_item(chunk_root);

    roots.emplace_back(BTRFS_ROOT_DEVTREE);
    root& devtree_root = roots.back();

    add_dev_stats(devtree_root);

    roots.emplace_back(BTRFS_ROOT_FSTREE);
    root& fstree_root = roots.back();

    populate_fstree(fstree_root);

    roots.emplace_back(BTRFS_ROOT_DATA_RELOC);
    populate_fstree(roots.back());

    roots.emplace_back(BTRFS_ROOT_CHECKSUM);
    root& csum_root = roots.back();

    root& image_subvol = add_image_subvol(root_root, fstree_root);

    parse_data_bitmap(dev, bmpdata, runs);

    // make sure runs don't go beyond end of device

    while (!runs.empty() && (runs.rbegin()->second.back().offset * cluster_size) + runs.rbegin()->second.back().length > device_size) {
        auto& r = runs.rbegin()->second;

        if (r.back().offset * cluster_size >= orig_device_size)
            r.pop_back();
        else {
            uint64_t len = orig_device_size - (r.back().offset * cluster_size);

            if (len % cluster_size)
                r.back().length = (len / cluster_size) + 1;
            else
                r.back().length = len / cluster_size;

            break;
        }
    }

    protect_superblocks(dev, runs);

    calc_used_space(runs, dev.boot_sector->BytesPerSector * dev.boot_sector->SectorsPerCluster);

    auto mftbmp = dev.mft->read(0, 0, ntfs_attribute::BITMAP);

    {
        ntfs_file secure(dev, NTFS_SECURE_INODE);

        create_inodes(fstree_root, mftbmp, dev, runs, secure, compression);
    }

    fmt::print("Mapped {} inodes directly.\n", mapped_inodes);
    fmt::print("Rewrote {} inodes.\n", rewritten_inodes);
    fmt::print("Inlined {} inodes.\n", inline_inodes);

    create_image(image_subvol, dev, runs, image_inode);

    roots.emplace_back(BTRFS_ROOT_UUID);
    add_subvol_uuid(roots.back());

    create_data_extent_items(extent_root, runs, dev.boot_sector->BytesPerSector * dev.boot_sector->SectorsPerCluster,
                             image_subvol.id, image_inode);

    fmt::print("Updating directory sizes\n");

    for (auto& r : roots) {
        if (!r.dir_size.empty())
            update_dir_sizes(r);
    }

    calc_checksums(csum_root, runs, dev, csum_type);

    populate_root_root(root_root);

    for (auto& r : roots) {
        if (r.id != BTRFS_ROOT_EXTENT && r.id != BTRFS_ROOT_CHUNK && r.id != BTRFS_ROOT_DEVTREE)
            r.create_trees(extent_root, csum_type);
    }

    do {
        bool extents_changed = false;

        chunks_changed = false;

        for (auto& c : chunks) {
            if (!c.added) {
                add_chunk(chunk_root, devtree_root, extent_root, c);
                c.added = true;
            }
        }

        for (auto& r : roots) {
            if (r.id == BTRFS_ROOT_EXTENT || r.id == BTRFS_ROOT_CHUNK || r.id == BTRFS_ROOT_DEVTREE) {
                r.old_addresses = r.addresses;
                r.addresses.clear();

                // FIXME - unallocate metadata and changed used value in chunks
                r.metadata_size -= r.trees.size() * tree_size;
                r.trees.clear();

                r.allocations_done = false;
                r.create_trees(extent_root, csum_type);

                if (r.allocations_done)
                    extents_changed = true;
            }
        }

        if (!chunks_changed && !extents_changed)
            break;
    } while (true);

    // update tree addresses and levels in-place in root 1
    update_root_root(root_root, csum_type);

    // update used value in BLOCK_GROUP_ITEMs
    update_extent_root(extent_root, csum_type);

    // update bytes_used in DEV_ITEM in root 3
    update_chunk_root(chunk_root, csum_type);

    for (auto& r : roots) {
        r.write_trees(dev);
    }

    write_superblocks(dev, chunk_root, root_root, compression, csum_type);

    clear_first_cluster(dev);
}

#if defined(__i386__) || defined(__x86_64__)
static void check_cpu() noexcept {
#ifndef _MSC_VER
    unsigned int cpuInfo[4];

    __get_cpuid(1, &cpuInfo[0], &cpuInfo[1], &cpuInfo[2], &cpuInfo[3]);

    if (cpuInfo[2] & bit_SSE4_2)
        calc_crc32c = calc_crc32c_hw;
#else
    int cpuInfo[4];

    __cpuid(cpuInfo, 1);

    if (cpuInfo[2] & (1 << 20))
        calc_crc32c = calc_crc32c_hw;
#endif
}
#endif

static enum btrfs_compression parse_compression_type(const string_view& s) {
    if (s == "none")
        return btrfs_compression::none;
    else if (s == "zlib")
        return btrfs_compression::zlib;
    else if (s == "lzo")
        return btrfs_compression::lzo;
    else if (s == "zstd")
        return btrfs_compression::zstd;
    else
        throw formatted_error("Unrecognized compression type {}.", s);
}

static enum btrfs_csum_type parse_csum_type(const string_view& s) {
    if (s == "crc32c")
        return btrfs_csum_type::crc32c;
    else if (s == "xxhash")
        return btrfs_csum_type::xxhash;
    else if (s == "sha256")
        return btrfs_csum_type::sha256;
    else if (s == "blake2")
        return btrfs_csum_type::blake2;
    else
        throw formatted_error("Unrecognized hash type {}.", s);
}

static vector<string_view> read_args(int argc, char* argv[]) {
    vector<string_view> ret;

    for (int i = 0; i < argc; i++) {
        ret.emplace_back(argv[i]);
    }

    return ret;
}

int main(int argc, char* argv[]) {
    try {
        auto args = read_args(argc, argv);

        if (args.size() == 2 && args[1] == "--version") {
            fmt::print("ntfs2btrfs " PROJECT_VER "\n");
            return 1;
        }

        if (args.size() < 2 || (args.size() == 2 && (args[1] == "--help" || args[1] == "/?"))) {
            fmt::print(R"(Usage: ntfs2btrfs [OPTION]... device
Convert an NTFS filesystem to Btrfs.

  -c, --compress=ALGO        recompress compressed files; ALGO can be 'zlib',
                               'lzo', 'zstd', or 'none'.
  -h, --hash=ALGO            checksum algorithm to use; ALGO can be 'crc32c'
                                (default), 'xxhash', 'sha256', or 'blake2'
  -r, --rollback             rollback to the original filesystem
)");
            return 1;
        }

        string fn;
        enum btrfs_compression compression;
        enum btrfs_csum_type csum_type;
        bool do_rollback = false;

#ifdef WITH_ZSTD
        compression = btrfs_compression::zstd;
#elif defined(WITH_LZO)
        compression = btrfs_compression::lzo;
#elif defined(WITH_ZLIB)
        compression = btrfs_compression::zlib;
#else
        compression = btrfs_compression::none;
#endif

        csum_type = btrfs_csum_type::crc32c;

        for (size_t i = 1; i < args.size(); i++) {
            const auto& arg = args[i];

            if (!arg.empty() && arg[0] == '-') {
                if (arg == "-c") {
                    if (i == args.size() - 1)
                        throw runtime_error("No value given for -c option.");

                    compression = parse_compression_type(args[i+1]);
                    i++;
                } else if (arg.substr(0, 11) == "--compress=")
                    compression = parse_compression_type(arg.substr(11));
                else if (arg == "-h") {
                    if (i == args.size() - 1)
                        throw runtime_error("No value given for -h option.");

                    csum_type = parse_csum_type(args[i+1]);
                    i++;
                } else if (arg.substr(0, 7) == "--hash=")
                    csum_type = parse_csum_type(arg.substr(11));
                else if (arg == "-r" || arg == "--rollback")
                    do_rollback = true;
                else
                    throw formatted_error("Unrecognized option {}.", arg);

            } else {
                if (!fn.empty())
                    throw runtime_error("Multiple devices given.");

                fn = arg;
            }
        }

        if (fn.empty())
            throw runtime_error("No device given.");

#if defined(__i386__) || defined(__x86_64__)
        check_cpu();
#endif

        if (do_rollback)
            rollback(fn);

#ifndef WITH_ZLIB
        if (compression == btrfs_compression::zlib)
            throw runtime_error("Zlib compression not compiled in.");
#endif

#ifndef WITH_LZO
        if (compression == btrfs_compression::lzo)
            throw runtime_error("LZO compression not compiled in.");
#endif

#ifndef WITH_ZSTD
        if (compression == btrfs_compression::zstd)
            throw runtime_error("Zstd compression not compiled in.");
#endif

        switch (compression) {
            case btrfs_compression::zlib:
                fmt::print("Using Zlib compression.\n");
                break;

            case btrfs_compression::lzo:
                fmt::print("Using LZO compression.\n");
                break;

            case btrfs_compression::zstd:
                fmt::print("Using Zstd compression.\n");
                break;

            case btrfs_compression::none:
                fmt::print("Not using compression.\n");
                break;
        }

        switch (csum_type) {
            case btrfs_csum_type::crc32c:
                fmt::print("Using CRC32C for checksums.\n");
                break;

            case btrfs_csum_type::xxhash:
                fmt::print("Using xxHash for checksums.\n");
                break;

            case btrfs_csum_type::sha256:
                fmt::print("Using SHA256 for checksums.\n");
                break;

            case btrfs_csum_type::blake2:
                fmt::print("Using Blake2 for checksums.\n");
                break;
        }

        ntfs dev(fn);

        convert(dev, compression, csum_type);
    } catch (const exception& e) {
        cerr << e.what() << endl;
        return 1;
    }

    return 0;
}
