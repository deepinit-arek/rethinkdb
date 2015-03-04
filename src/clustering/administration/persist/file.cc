// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "clustering/administration/persist/file.hpp"

#include "btree/depth_first_traversal.hpp"
#include "btree/types.hpp"
#include "buffer_cache/blob.hpp"
#include "buffer_cache/cache_balancer.hpp"
#include "buffer_cache/serialize_onto_blob.hpp"
#include "clustering/administration/persist/migrate_v1_16.hpp"
#include "serializer/log/log_serializer.hpp"

struct metadata_disk_superblock_t {
    block_magic_t magic;
    
    block_id_t root_block;
    block_id_t stat_block;
} __attribute__ ((packed));

// Etymology: In version 1.13, the magic was 'RDmd', for "(R)ethink(D)B (m)eta(d)ata".
// Every subsequent version, the last character has been incremented.
static const block_magic_t metadata_sb_magic = { { 'R', 'D', 'm', 'h' } };

void init_metadata_superblock(void *sb_void, size_t block_size) {
    memset(sb_void, 0, block_size);
    metadata_disk_superblock_t *sb = static_cast<metadata_disk_superblock_t *>(sb_void);
    sb->magic = metadata_sb_magic;
    sb->root_block = NULL_BLOCK_ID;
    sb->stat_block = NULL_BLOCK_ID;
}

cluster_version_t magic_to_version(block_magic_t magic) {
    guarantee(magic.bytes[0] == metadata_sb_magic.bytes[0]);
    guarantee(magic.bytes[1] == metadata_sb_magic.bytes[1]);
    guarantee(magic.bytes[2] == metadata_sb_magic.bytes[2]);
    switch (magic.bytes[3]) {
        case 'd': return cluster_version_t::v1_13;
        case 'e': return cluster_version_t::v1_14;
        case 'f': return cluster_version_t::v1_15;
        case 'g': return cluster_version_t::v1_16;
        case 'h': return cluster_version_t::raft_is_latest_disk;
        default: crash("You're trying to use an earlier version of RethinkDB to open a "
            "database created by a later version of RethinkDB.");
    }
}

class metadata_superblock_t : public superblock_t {
public:
    metadata_superblock_t(buf_lock_t &&sb_buf) : sb_buf_(std::move(sb_buf)) { }
    void release() {
        sb_buf_.reset_buf_lock();
    }
    block_id_t get_root_block_id() {
        buf_read_t read(&sb_buf_);
        auto ptr = static_cast<const metadata_disk_superblock_t *>(read.get_data_read());
        return ptr->root_block;
    }
    void set_root_block_id(const block_id_t new_root_block) {
        buf_write_t write(&sb_buf_);
        auto ptr = static_cast<metadata_disk_superblock_t *>(write.get_data_write());
        ptr->root_block = new_root_block;
    }
    block_id_t get_stat_block_id() {
        buf_read_t read(&sb_buf_);
        auto ptr = static_cast<const metadata_disk_superblock_t *>(read.get_data_read());
        return ptr->stat_block;
    }
    void set_stat_block_id(const block_id_t new_stat_block) {
        buf_write_t write(&sb_buf_);
        auto ptr = static_cast<metadata_disk_superblock_t *>(write.get_data_write());
        ptr->stat_block = new_stat_block;
    }
    buf_parent_t expose_buf() {
        return buf_parent_t(&sb_buf_);
    }
private:
    buf_lock_t sb_buf_;
};

class metadata_value_sizer_t : public value_sizer_t {
public:
    explicit metadata_value_sizer_t(max_block_size_t _bs) : bs(_bs) { }
    int size(const void *value) const {
        return blob::ref_size(
            bs,
            static_cast<const char *>(value),
            blob::btree_maxreflen);
    }
    bool fits(const void *value, int length_available) const {
        return blob::ref_fits(
            bs,
            length_available,
            static_cast<const char *>(value),
            blob::btree_maxreflen);
    }
    int max_possible_size() const {
        return blob::btree_maxreflen;
    }
    block_magic_t btree_leaf_magic() const {
        return block_magic_t { { 'R', 'D', 'l', 'n' } };
    }
    max_block_size_t block_size() const {
        return bs;
    }
private:
    max_block_size_t bs;
};

class metadata_value_deleter_t : public value_deleter_t {
public:
    void delete_value(buf_parent_t parent, const void *value) const {
        // To not destroy constness, we operate on a copy of the value
        metadata_value_sizer_t sizer(parent.cache()->max_block_size());
        scoped_malloc_t<void> value_copy(sizer.max_possible_size());
        memcpy(value_copy.get(), value, sizer.size(value));
        blob_t blob(
            parent.cache()->max_block_size(),
            static_cast<char *>(value_copy.get()),
            blob::btree_maxreflen);
        blob.clear(parent);
    }
};

metadata_file_t::read_txn_t::read_txn_t(
        metadata_file_t *f,
        signal_t *interruptor) :
    file(f),
    rwlock_acq(&file->rwlock, access_t::read, interruptor),
    txn(file->cache_conn.get(), read_access_t::read)
    { }

metadata_file_t::read_txn_t::read_txn_t(
        metadata_file_t *f,
        write_access_t,
        signal_t *interruptor) :
    file(f),
    rwlock_acq(&file->rwlock, access_t::write, interruptor),
    txn(file->cache_conn.get(), write_durability_t::HARD,
        repli_timestamp_t::distant_past, 1)
    { }

void metadata_file_t::read_txn_t::blob_to_stream(
        buf_parent_t parent,
        const void *ref,
        const std::function<void(read_stream_t *)> &callback) {
    blob_t blob(
        file->cache->max_block_size(),
        /* `blob_t` requires a non-const pointer because it has functions that mutate the
        blob. But we're not using those functions. That's why there's a `const_cast`
        here. */
        static_cast<char *>(const_cast<void *>(ref)),
        blob::btree_maxreflen);
    blob_acq_t acq_group;
    buffer_group_t buf_group;
    blob.expose_all(parent, access_t::read, &buf_group, &acq_group);
    buffer_group_read_stream_t read_stream(const_view(&buf_group));
    callback(&read_stream);
}

void metadata_file_t::read_txn_t::read_bin(
        const store_key_t &key,
        const std::function<void(read_stream_t *)> &callback,
        signal_t *interruptor) {
    metadata_value_sizer_t sizer(file->cache->max_block_size());
    buf_lock_t sb_lock(buf_parent_t(&txn), SUPERBLOCK_ID, access_t::read);
    wait_interruptible(sb_lock.read_acq_signal(), interruptor);
    metadata_superblock_t superblock(std::move(sb_lock));
    keyvalue_location_t kvloc;
    find_keyvalue_location_for_read(
        &sizer,
        &superblock,
        key.btree_key(),
        &kvloc,
        &file->btree_stats,
        nullptr);
    if (kvloc.there_originally_was_value) {
        blob_to_stream(buf_parent_t(&kvloc.buf), kvloc.value.get(), callback);
    }
}

void metadata_file_t::read_txn_t::read_many_bin(
        const store_key_t &key_prefix,
        const std::function<void(const std::string &key_suffix, read_stream_t *)> &cb,
        signal_t *interruptor) {
    buf_lock_t sb_lock(buf_parent_t(&txn), SUPERBLOCK_ID, access_t::read);
    wait_interruptible(sb_lock.read_acq_signal(), interruptor);
    metadata_superblock_t superblock(std::move(sb_lock));
    class : public depth_first_traversal_callback_t {
    public:
        done_traversing_t handle_pair(scoped_key_value_t &&kv) {
            guarantee(kv.key()->size >= key_prefix.size());
            guarantee(memcmp(
                kv.key()->contents, key_prefix.contents(), key_prefix.size()) == 0);
            std::string suffix(
                reinterpret_cast<const char *>(kv.key()->contents + key_prefix.size()),
                kv.key()->size - key_prefix.size());
            txn->blob_to_stream(
                kv.expose_buf(),
                kv.value(),
                [&](read_stream_t *s) { (*cb)(suffix, s); });
            return interruptor->is_pulsed()
                ? done_traversing_t::YES
                : done_traversing_t::NO;
        }
        read_txn_t *txn;
        store_key_t key_prefix;
        const std::function<void(const std::string &key_suffix, read_stream_t *)> *cb;
        signal_t *interruptor;
    } dftcb;
    dftcb.txn = this;
    dftcb.key_prefix = key_prefix;
    dftcb.cb = &cb;
    dftcb.interruptor = interruptor;
    btree_depth_first_traversal(
        &superblock,
        key_range_t::with_prefix(key_prefix),
        &dftcb,
        FORWARD,
        release_superblock_t::RELEASE);
    if (interruptor->is_pulsed()) {
        throw interrupted_exc_t();
    }
}

metadata_file_t::write_txn_t::write_txn_t(
        metadata_file_t *file,
        signal_t *interruptor) :
    read_txn_t(file, write_access_t::write, interruptor)
    { }

void metadata_file_t::write_txn_t::write_bin(
        const store_key_t &key,
        const write_message_t *msg,
        signal_t *interruptor) {
    metadata_value_sizer_t sizer(file->cache->max_block_size());
    metadata_value_deleter_t deleter;
    buf_lock_t sb_lock(buf_parent_t(&txn), SUPERBLOCK_ID, access_t::write);
    wait_interruptible(sb_lock.write_acq_signal(), interruptor);
    metadata_superblock_t superblock(std::move(sb_lock));
    keyvalue_location_t kvloc;
    find_keyvalue_location_for_write(
        &sizer,
        &superblock,
        key.btree_key(),
        &deleter,
        &kvloc,
        &file->btree_stats,
        nullptr);
    if (kvloc.there_originally_was_value) {
        deleter.delete_value(buf_parent_t(&kvloc.buf), kvloc.value.get());
        kvloc.value.reset();
    }
    if (msg != nullptr) {
        kvloc.value = scoped_malloc_t<void>(blob::btree_maxreflen);
        memset(kvloc.value.get(), 0, blob::btree_maxreflen);
        blob_t blob(file->cache->max_block_size(),
                    static_cast<char *>(kvloc.value.get()),
                    blob::btree_maxreflen);
        write_onto_blob(buf_parent_t(&kvloc.buf), &blob, *msg);
    }
    null_key_modification_callback_t null_cb;
    apply_keyvalue_change(&sizer, &kvloc, key.btree_key(), repli_timestamp_t::invalid,
        &deleter, &null_cb, delete_or_erase_t::ERASE);
}

metadata_file_t::metadata_file_t(
        io_backender_t *io_backender,
        const serializer_filepath_t &filename,
        perfmon_collection_t *perfmon_parent,
        signal_t *interruptor) :
    btree_stats(perfmon_parent, "metadata")
{
    filepath_file_opener_t file_opener(filename, io_backender);
    serializer.init(new standard_serializer_t(
        standard_serializer_t::dynamic_config_t(),
        &file_opener,
        perfmon_parent));
    if (!serializer->coop_lock_and_check()) {
        throw file_in_use_exc_t();
    }
    balancer.init(new dummy_cache_balancer_t(MEGABYTE));
    cache.init(new cache_t(serializer.get(), balancer.get(), perfmon_parent));
    cache_conn.init(new cache_conn_t(cache.get()));

    /* Migrate data if necessary */
    write_txn_t write_txn(this, interruptor);
    object_buffer_t<buf_lock_t> sb_lock;
    sb_lock.create(buf_parent_t(&write_txn.txn), SUPERBLOCK_ID, access_t::write);
    object_buffer_t<buf_write_t> sb_write;
    sb_write.create(sb_lock.get());
    void *sb_data = sb_write->get_data_write();
    cluster_version_t metadata_version =
        magic_to_version(*static_cast<block_magic_t *>(sb_data));
    switch (metadata_version) {
        case cluster_version_t::v1_13:
        case cluster_version_t::v1_13_2:
        case cluster_version_t::v1_14:
        case cluster_version_t::v1_15: {
            crash("This version of RethinkDB cannot migrate in place from databases "
                "created by versions older than RethinkDB 1.16.");
            break;
        }
        case cluster_version_t::v1_16: {
            scoped_malloc_t<void> sb_copy(cache->max_block_size().value());
            memcpy(sb_copy.get(), sb_data, cache->max_block_size().value());
            init_metadata_superblock(sb_data, cache->max_block_size().value());
            sb_write.reset();
            sb_lock.reset();
            migrate_v1_16::migrate_cluster_metadata(
                &write_txn.txn, buf_parent_t(&write_txn.txn), sb_copy.get(), &write_txn);
            break;
        }
        case cluster_version_t::raft_is_latest: {
            /* No need to do any migration */
            break;
        }
        default: unreachable();
    }
}

metadata_file_t::metadata_file_t(
        io_backender_t *io_backender,
        const serializer_filepath_t &filename,
        perfmon_collection_t *perfmon_parent,
        const std::function<void(write_txn_t *, signal_t *)> &initializer,
        signal_t *interruptor) :
    btree_stats(perfmon_parent, "metadata")
{
    filepath_file_opener_t file_opener(filename, io_backender);
    standard_serializer_t::create(
        &file_opener,
        standard_serializer_t::static_config_t());
    serializer.init(new standard_serializer_t(
        standard_serializer_t::dynamic_config_t(),
        &file_opener,
        perfmon_parent));
    if (!serializer->coop_lock_and_check()) {
        throw file_in_use_exc_t();
    }
    balancer.init(new dummy_cache_balancer_t(MEGABYTE));
    cache.init(new cache_t(serializer.get(), balancer.get(), perfmon_parent));
    cache_conn.init(new cache_conn_t(cache.get()));

    {
        write_txn_t write_txn(this, interruptor);
        {
            buf_lock_t sb_lock(&write_txn.txn, SUPERBLOCK_ID, alt_create_t::create);
            buf_write_t sb_write(&sb_lock);
            void *sb_data = sb_write.get_data_write();
            init_metadata_superblock(sb_data, cache->max_block_size().value());
        }
        initializer(&write_txn, interruptor);
    }

    file_opener.move_serializer_file_to_permanent_location();
}

metadata_file_t::~metadata_file_t() {
    /* This is defined in the `.cc` file so the `.hpp` file doesn't need to see the
    definitions of `log_serializer_t` and `cache_balancer_t`. */
}
