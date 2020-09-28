#include "CacheDictionary.h"

#include <memory>
#include <Columns/ColumnString.h>
#include <Common/BitHelpers.h>
#include <Common/CurrentMetrics.h>
#include <Common/HashTable/Hash.h>
#include <Common/ProfileEvents.h>
#include <Common/ProfilingScopedRWLock.h>
#include <Common/randomSeed.h>
#include <Common/typeid_cast.h>
#include <Core/Defines.h>
#include <ext/range.h>
#include <ext/size.h>
#include <Common/setThreadName.h>
#include "CacheDictionary.inc.h"
#include "DictionaryBlockInputStream.h"
#include "DictionaryFactory.h"


namespace ProfileEvents
{
extern const Event DictCacheKeysRequested;
extern const Event DictCacheKeysRequestedMiss;
extern const Event DictCacheKeysRequestedFound;
extern const Event DictCacheKeysExpired;
extern const Event DictCacheKeysNotFound;
extern const Event DictCacheKeysHit;
extern const Event DictCacheRequestTimeNs;
extern const Event DictCacheRequests;
extern const Event DictCacheLockWriteNs;
extern const Event DictCacheLockReadNs;
}

namespace CurrentMetrics
{
extern const Metric DictCacheRequests;
}


namespace DB
{
namespace ErrorCodes
{
    extern const int CACHE_DICTIONARY_UPDATE_FAIL;
    extern const int TYPE_MISMATCH;
    extern const int BAD_ARGUMENTS;
    extern const int UNSUPPORTED_METHOD;
    extern const int TOO_SMALL_BUFFER_SIZE;
    extern const int TIMEOUT_EXCEEDED;
}


inline size_t CacheDictionary::getCellIdx(const Key id) const
{
    const auto hash = intHash64(id);
    const auto idx = hash & size_overlap_mask;
    return idx;
}


CacheDictionary::CacheDictionary(
    const StorageID & dict_id_,
    const DictionaryStructure & dict_struct_,
    DictionarySourcePtr source_ptr_,
    DictionaryLifetime dict_lifetime_,
    size_t strict_max_lifetime_seconds_,
    size_t size_,
    bool allow_read_expired_keys_,
    size_t max_update_queue_size_,
    size_t update_queue_push_timeout_milliseconds_,
    size_t query_wait_timeout_milliseconds_,
    size_t max_threads_for_updates_)
    : IDictionary(dict_id_)
    , dict_struct(dict_struct_)
    , source_ptr{std::move(source_ptr_)}
    , dict_lifetime(dict_lifetime_)
    , strict_max_lifetime_seconds(strict_max_lifetime_seconds_)
    , allow_read_expired_keys(allow_read_expired_keys_)
    , max_update_queue_size(max_update_queue_size_)
    , update_queue_push_timeout_milliseconds(update_queue_push_timeout_milliseconds_)
    , query_wait_timeout_milliseconds(query_wait_timeout_milliseconds_)
    , max_threads_for_updates(max_threads_for_updates_)
    , log(&Poco::Logger::get("ExternalDictionaries"))
    , size{roundUpToPowerOfTwoOrZero(std::max(size_, size_t(max_collision_length)))}
    , size_overlap_mask{this->size - 1}
    , cells{this->size}
    , cache{size_}
    , rnd_engine(randomSeed())
    , update_queue(max_update_queue_size_)
    , update_pool(max_threads_for_updates)
{
    if (!source_ptr->supportsSelectiveLoad())
        throw Exception{full_name + ": source cannot be used with CacheDictionary", ErrorCodes::UNSUPPORTED_METHOD};

    createAttributes();
    for (size_t i = 0; i < max_threads_for_updates; ++i)
        update_pool.scheduleOrThrowOnError([this] { updateThreadFunction(); });
}

CacheDictionary::~CacheDictionary()
{
    finished = true;
    update_queue.clear();
    for (size_t i = 0; i < max_threads_for_updates; ++i)
    {
        auto empty_finishing_ptr = std::make_shared<UpdateUnit>(std::vector<Key>());
        update_queue.push(empty_finishing_ptr);
    }
    update_pool.wait();
}

size_t CacheDictionary::getBytesAllocated() const
{
    /// In case of existing string arena we check the size of it.
    /// But the same appears in setAttributeValue() function, which is called from update() function
    /// which in turn is called from another thread.
    const ProfilingScopedReadRWLock read_lock{rw_lock, ProfileEvents::DictCacheLockReadNs};
    return bytes_allocated + (string_arena ? string_arena->size() : 0);
}

const IDictionarySource * CacheDictionary::getSource() const
{
    /// Mutex required here because of the getSourceAndUpdateIfNeeded() function
    /// which is used from another thread.
    std::lock_guard lock(source_mutex);
    return source_ptr.get();
}

void CacheDictionary::toParent(const PaddedPODArray<Key> & ids, PaddedPODArray<Key> & out) const
{
    const auto null_value = std::get<UInt64>(hierarchical_attribute->null_values);

    getItemsNumberImpl<UInt64, UInt64>(*hierarchical_attribute, ids, out, [&](const size_t) { return null_value; });
}


/// Allow to use single value in same way as array.
static inline CacheDictionary::Key getAt(const PaddedPODArray<CacheDictionary::Key> & arr, const size_t idx)
{
    return arr[idx];
}
static inline CacheDictionary::Key getAt(const CacheDictionary::Key & value, const size_t)
{
    return value;
}


template <typename AncestorType>
void CacheDictionary::isInImpl(const PaddedPODArray<Key> & child_ids, const AncestorType & ancestor_ids, PaddedPODArray<UInt8> & out) const
{
    /// Transform all children to parents until ancestor id or null_value will be reached.

    size_t out_size = out.size();
    memset(out.data(), 0xFF, out_size); /// 0xFF means "not calculated"

    const auto null_value = std::get<UInt64>(hierarchical_attribute->null_values);

    PaddedPODArray<Key> children(out_size, 0);
    PaddedPODArray<Key> parents(child_ids.begin(), child_ids.end());

    for (size_t i = 0; i < DBMS_HIERARCHICAL_DICTIONARY_MAX_DEPTH; ++i)
    {
        size_t out_idx = 0;
        size_t parents_idx = 0;
        size_t new_children_idx = 0;

        while (out_idx < out_size)
        {
            /// Already calculated
            if (out[out_idx] != 0xFF)
            {
                ++out_idx;
                continue;
            }

            /// No parent
            if (parents[parents_idx] == null_value)
            {
                out[out_idx] = 0;
            }
            /// Found ancestor
            else if (parents[parents_idx] == getAt(ancestor_ids, parents_idx))
            {
                out[out_idx] = 1;
            }
            /// Loop detected
            else if (children[new_children_idx] == parents[parents_idx])
            {
                out[out_idx] = 1;
            }
            /// Found intermediate parent, add this value to search at next loop iteration
            else
            {
                children[new_children_idx] = parents[parents_idx];
                ++new_children_idx;
            }

            ++out_idx;
            ++parents_idx;
        }

        if (new_children_idx == 0)
            break;

        /// Transform all children to its parents.
        children.resize(new_children_idx);
        parents.resize(new_children_idx);

        toParent(children, parents);
    }
}

void CacheDictionary::isInVectorVector(
    const PaddedPODArray<Key> & child_ids, const PaddedPODArray<Key> & ancestor_ids, PaddedPODArray<UInt8> & out) const
{
    isInImpl(child_ids, ancestor_ids, out);
}

void CacheDictionary::isInVectorConstant(const PaddedPODArray<Key> & child_ids, const Key ancestor_id, PaddedPODArray<UInt8> & out) const
{
    isInImpl(child_ids, ancestor_id, out);
}

void CacheDictionary::isInConstantVector(const Key child_id, const PaddedPODArray<Key> & ancestor_ids, PaddedPODArray<UInt8> & out) const
{
    /// Special case with single child value.

    const auto null_value = std::get<UInt64>(hierarchical_attribute->null_values);

    PaddedPODArray<Key> child(1, child_id);
    PaddedPODArray<Key> parent(1);
    std::vector<Key> ancestors(1, child_id);

    /// Iteratively find all ancestors for child.
    for (size_t i = 0; i < DBMS_HIERARCHICAL_DICTIONARY_MAX_DEPTH; ++i)
    {
        toParent(child, parent);

        if (parent[0] == null_value)
            break;

        child[0] = parent[0];
        ancestors.push_back(parent[0]);
    }

    /// Assuming short hierarchy, so linear search is Ok.
    for (size_t i = 0, out_size = out.size(); i < out_size; ++i)
        out[i] = std::find(ancestors.begin(), ancestors.end(), ancestor_ids[i]) != ancestors.end();
}

void CacheDictionary::getString(const std::string & attribute_name, const PaddedPODArray<Key> & ids, ColumnString * out) const
{
    auto & attribute = getAttribute(attribute_name);
    checkAttributeType(this, attribute_name, attribute.type, AttributeUnderlyingType::utString);

    const auto null_value = StringRef{std::get<String>(attribute.null_values)};

    getItemsString(attribute, ids, out, [&](const size_t) { return null_value; });
}

void CacheDictionary::getString(
    const std::string & attribute_name, const PaddedPODArray<Key> & ids, const ColumnString * const def, ColumnString * const out) const
{
    auto & attribute = getAttribute(attribute_name);
    checkAttributeType(this, attribute_name, attribute.type, AttributeUnderlyingType::utString);

    getItemsString(attribute, ids, out, [&](const size_t row) { return def->getDataAt(row); });
}

void CacheDictionary::getString(
    const std::string & attribute_name, const PaddedPODArray<Key> & ids, const String & def, ColumnString * const out) const
{
    auto & attribute = getAttribute(attribute_name);
    checkAttributeType(this, attribute_name, attribute.type, AttributeUnderlyingType::utString);

    getItemsString(attribute, ids, out, [&](const size_t) { return StringRef{def}; });
}

template<class... Ts>
struct Overloaded : Ts... {using Ts::operator()...;};

template<class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

std::string CacheDictionary::AttributeValuesForKey::dump()
{
    std::ostringstream os;
    for (auto & attr : values)
        std::visit(Overloaded {
            [&os](UInt8 arg)   { os << "type: UInt8, value: "   <<  std::to_string(arg) << "\n"; },
            [&os](UInt16 arg)  { os << "type: UInt16, value: "  <<  std::to_string(arg) << "\n"; },
            [&os](UInt32 arg)  { os << "type: UInt32, value: "  <<  std::to_string(arg) << "\n"; },
            [&os](UInt64 arg)  { os << "type: UInt64, value: "  <<  std::to_string(arg) << "\n"; },
            [&os](UInt128 arg) { os << "type: UInt128, value: " << arg.toHexString() << "\n"; },
            [&os](Int8 arg)   { os << "type: Int8, value: "   <<  std::to_string(arg) << "\n"; },
            [&os](Int16 arg)  { os << "type: Int16, value: "  <<  std::to_string(arg) << "\n"; },
            [&os](Int32 arg)  { os << "type: Int32, value: "  <<  std::to_string(arg) << "\n"; },
            [&os](Int64 arg)  { os << "type: Int64, value: "  <<  std::to_string(arg) << "\n"; },
            [&os](Decimal32 arg)   { os << "type: Decimal32, value: "  <<  std::to_string(arg) << "\n"; },
            [&os](Decimal64 arg)   { os << "type: Decimal64, value: "  <<  std::to_string(arg) << "\n"; },
            [&os](Decimal128)  { os << "type: Decimal128, value: ???" << "\n" ; },
            [&os](Float32 arg)   { os << "type: Float32, value: "  <<  std::to_string(arg) << "\n"; },
            [&os](Float64 arg)   { os << "type: Float64, value: "  <<  std::to_string(arg) << "\n"; },
            [&os](String arg)  { os << "type: String, value: " <<  arg + "\n"; }
        }, attr);
    return os.str();
};


std::string CacheDictionary::UpdateUnit::dumpFoundIds()
{
    std::string ans;
    for (auto it : found_ids)
    {
        ans += "Key: " + std::to_string(it.first) + "\n";
        if (it.second.found)
            ans += it.second.dump() + "\n";
    }
    return ans;
};


CacheDictionary::FindResult CacheDictionary::findCell(const Key & id, const time_point_t now) const
{
    auto result = cache.get(id);
    if (!result)
        return {result, false, false};
    
    if (result->deadline > now)
        return {result, true, true};

    return {result, true, false};
}

void CacheDictionary::has(const PaddedPODArray<Key> & ids, PaddedPODArray<UInt8> & out) const
{
    /// There are three types of ids.
    /// - Valid ids. These ids are presented in local cache and their lifetime is not expired.
    /// - CacheExpired ids. Ids that are in local cache, but their values are rotted (lifetime is expired).
    /// - CacheNotFound ids. We have to go to external storage to know its value.

    /// Mapping: <id> -> { all indices `i` of `ids` such that `ids[i]` = <id> }
    std::unordered_map<Key, std::vector<size_t>> cache_expired_or_not_found_ids;

    size_t cache_hit = 0;

    size_t cache_expired_count = 0;
    size_t cache_not_found_count = 0;

    const auto rows = ext::size(ids);
    {
        const ProfilingScopedReadRWLock read_lock{rw_lock, ProfileEvents::DictCacheLockReadNs};

        const auto now = std::chrono::system_clock::now();
        /// fetch up-to-date values, decide which ones require update
        for (const auto row : ext::range(0, rows))
        {
            const auto id = ids[row];
            const auto find_result = findCellIdx(id, now);
            const auto & cell_idx = find_result.cell_idx;

            auto insert_to_answer_routine = [&] ()
            {
                out[row] = !cells[cell_idx].isDefault();
            };

            if (!find_result.valid)
            {
                if (find_result.outdated)
                {
                    /// Protection of reading very expired keys.
                    if (now > cells[find_result.cell_idx].strict_max)
                    {
                        cache_not_found_count++;
                        cache_expired_or_not_found_ids[id].push_back(row);
                        continue;
                    }
                    cache_expired_count++;
                    cache_expired_or_not_found_ids[id].push_back(row);

                    if (allow_read_expired_keys)
                        insert_to_answer_routine();
                }
                else
                {
                    cache_not_found_count++;
                    cache_expired_or_not_found_ids[id].push_back(row);
                }
            }
            else
            {
                ++cache_hit;
                insert_to_answer_routine();
            }
        }
    }

    ProfileEvents::increment(ProfileEvents::DictCacheKeysExpired, cache_expired_count);
    ProfileEvents::increment(ProfileEvents::DictCacheKeysNotFound, cache_not_found_count);
    ProfileEvents::increment(ProfileEvents::DictCacheKeysHit, cache_hit);

    query_count.fetch_add(rows, std::memory_order_relaxed);
    hit_count.fetch_add(rows - cache_expired_count - cache_not_found_count, std::memory_order_release);

    if (!cache_not_found_count)
    {
        /// Nothing to update - return;
        if (!cache_expired_count)
            return;

        if (allow_read_expired_keys)
        {
            std::vector<Key> required_expired_ids;
            required_expired_ids.reserve(cache_expired_count);
            std::transform(
                    std::begin(cache_expired_or_not_found_ids), std::end(cache_expired_or_not_found_ids),
                    std::back_inserter(required_expired_ids), [](auto & pair) { return pair.first; });

            auto update_unit_ptr = std::make_shared<UpdateUnit>(std::move(required_expired_ids));

            tryPushToUpdateQueueOrThrow(update_unit_ptr);
            /// Update is async - no need to wait.
            return;
        }
    }

    /// At this point we have two situations.
    /// There may be both types of keys: cache_expired_ids and cache_not_found_ids.
    /// We will update them all synchronously.

    std::vector<Key> required_ids;
    required_ids.reserve(cache_not_found_count + cache_expired_count);
    std::transform(
            std::begin(cache_expired_or_not_found_ids), std::end(cache_expired_or_not_found_ids),
            std::back_inserter(required_ids), [](auto & pair) { return pair.first; });

    auto update_unit_ptr = std::make_shared<UpdateUnit>(std::move(required_ids));

    tryPushToUpdateQueueOrThrow(update_unit_ptr);
    waitForCurrentUpdateFinish(update_unit_ptr);

    for (auto & [key, value] : update_unit_ptr->found_ids)
    {
        if (value.found)
            for (const auto row : cache_expired_or_not_found_ids[key])
                out[row] = true;
        else
            for (const auto row : cache_expired_or_not_found_ids[key])
                out[row] = false;
    }
}


void CacheDictionary::createAttributes()
{
    const auto attributes_size = dict_struct.attributes.size();

    for (const auto & attribute : dict_struct.attributes)
    {
        attribute_index_by_name.emplace(attribute.name, attributes.size());

        if (attribute.hierarchical)
        {
            hierarchical_attribute = &attributes.back();

            if (hierarchical_attribute->type != AttributeUnderlyingType::utUInt64)
                throw Exception{full_name + ": hierarchical attribute must be UInt64.", ErrorCodes::TYPE_MISMATCH};
        }
    }
}

CacheDictionary::Attribute CacheDictionary::createAttributeWithTypeAndName(const AttributeUnderlyingType type, const String & name, const Field & null_value)
{
    Attribute attr{type, name, {}, {}};

    switch (type)
    {
#define DISPATCH(TYPE) \
    case AttributeUnderlyingType::ut##TYPE: \
        attr.null_values = TYPE(null_value.get<NearestFieldType<TYPE>>()); /* NOLINT */ \
        attr.arrays = std::make_unique<ContainerType<TYPE>>(size); /* NOLINT */ \
        bytes_allocated += size * sizeof(TYPE); \
        break;
        DISPATCH(UInt8)
        DISPATCH(UInt16)
        DISPATCH(UInt32)
        DISPATCH(UInt64)
        DISPATCH(UInt128)
        DISPATCH(Int8)
        DISPATCH(Int16)
        DISPATCH(Int32)
        DISPATCH(Int64)
        DISPATCH(Decimal32)
        DISPATCH(Decimal64)
        DISPATCH(Decimal128)
        DISPATCH(Float32)
        DISPATCH(Float64)
#undef DISPATCH
        case AttributeUnderlyingType::utString:
            attr.null_values = null_value.get<String>();
            attr.arrays = std::make_unique<ContainerType<StringRef>>(size);
            bytes_allocated += size * sizeof(StringRef);
            if (!string_arena)
                string_arena = std::make_unique<ArenaWithFreeLists>();
            break;
    }

    return attr;
}

void CacheDictionary::setDefaultAttributeValue(Attribute & attribute, const Key idx) const
{
    switch (attribute.type)
    {
#define DISPATCH(TYPE) \
        case AttributeUnderlyingType::ut##TYPE: \
            std::get<ContainerPtrType<TYPE>>(attribute.arrays)[idx] = std::get<TYPE>(attribute.null_values); /* NOLINT */ \
            break;
        DISPATCH(UInt8)
        DISPATCH(UInt16)
        DISPATCH(UInt32)
        DISPATCH(UInt64)
        DISPATCH(UInt128)
        DISPATCH(Int8)
        DISPATCH(Int16)
        DISPATCH(Int32)
        DISPATCH(Int64)
        DISPATCH(Decimal32)
        DISPATCH(Decimal64)
        DISPATCH(Decimal128)
        DISPATCH(Float32)
        DISPATCH(Float64)
#undef DISPATCH
        case AttributeUnderlyingType::utString:
        {
            const auto & null_value_ref = std::get<String>(attribute.null_values);
            auto & string_ref = std::get<ContainerPtrType<StringRef>>(attribute.arrays)[idx];

            if (string_ref.data != null_value_ref.data())
            {
                if (string_ref.data)
                    string_arena->free(const_cast<char *>(string_ref.data), string_ref.size);

                string_ref = StringRef{null_value_ref};
            }

            break;
        }
    }
}


CacheDictionary::Attribute & CacheDictionary::getAttribute(const std::string & attribute_name) const
{
    const size_t attr_index = getAttributeIndex(attribute_name);
    return attributes[attr_index];
}

size_t CacheDictionary::getAttributeIndex(const std::string & attribute_name) const
{
    const auto it = attribute_index_by_name.find(attribute_name);
    if (it == std::end(attribute_index_by_name))
        throw Exception{full_name + ": no such attribute '" + attribute_name + "'", ErrorCodes::BAD_ARGUMENTS};

    return it->second;
}

bool CacheDictionary::isEmptyCell(const UInt64 idx) const
{
    return (idx != zero_cell_idx && cells[idx].id == 0)
        || (cells[idx].data == ext::safe_bit_cast<CellMetadata::time_point_urep_t>(CellMetadata::time_point_t()));
}

PaddedPODArray<CacheDictionary::Key> CacheDictionary::getCachedIds() const
{
    const ProfilingScopedReadRWLock read_lock{rw_lock, ProfileEvents::DictCacheLockReadNs};

    PaddedPODArray<Key> array;
    for (size_t idx = 0; idx < cells.size(); ++idx)
    {
        auto & cell = cells[idx];
        if (!isEmptyCell(idx) && !cells[idx].isDefault())
        {
            array.push_back(cell.id);
        }
    }
    return array;
}

BlockInputStreamPtr CacheDictionary::getBlockInputStream(const Names & column_names, size_t max_block_size) const
{
    using BlockInputStreamType = DictionaryBlockInputStream<CacheDictionary, Key>;
    return std::make_shared<BlockInputStreamType>(shared_from_this(), max_block_size, getCachedIds(), column_names);
}

std::exception_ptr CacheDictionary::getLastException() const
{
    const ProfilingScopedReadRWLock read_lock{rw_lock, ProfileEvents::DictCacheLockReadNs};
    return last_exception;
}

void registerDictionaryCache(DictionaryFactory & factory)
{
    auto create_layout = [=](const std::string & full_name,
                             const DictionaryStructure & dict_struct,
                             const Poco::Util::AbstractConfiguration & config,
                             const std::string & config_prefix,
                             DictionarySourcePtr source_ptr) -> DictionaryPtr
    {
        if (dict_struct.key)
            throw Exception{"'key' is not supported for dictionary of layout 'cache'",
                            ErrorCodes::UNSUPPORTED_METHOD};

        if (dict_struct.range_min || dict_struct.range_max)
            throw Exception{full_name
                                + ": elements .structure.range_min and .structure.range_max should be defined only "
                                  "for a dictionary of layout 'range_hashed'",
                            ErrorCodes::BAD_ARGUMENTS};
        const auto & layout_prefix = config_prefix + ".layout";

        const size_t size = config.getUInt64(layout_prefix + ".cache.size_in_cells");
        if (size == 0)
            throw Exception{full_name + ": dictionary of layout 'cache' cannot have 0 cells",
                            ErrorCodes::TOO_SMALL_BUFFER_SIZE};

        const bool require_nonempty = config.getBool(config_prefix + ".require_nonempty", false);
        if (require_nonempty)
            throw Exception{full_name + ": dictionary of layout 'cache' cannot have 'require_nonempty' attribute set",
                            ErrorCodes::BAD_ARGUMENTS};

        const auto dict_id = StorageID::fromDictionaryConfig(config, config_prefix);
        const DictionaryLifetime dict_lifetime{config, config_prefix + ".lifetime"};

        const size_t strict_max_lifetime_seconds =
                config.getUInt64(layout_prefix + ".cache.strict_max_lifetime_seconds", static_cast<size_t>(dict_lifetime.max_sec));

        const size_t max_update_queue_size =
                config.getUInt64(layout_prefix + ".cache.max_update_queue_size", 100000);
        if (max_update_queue_size == 0)
            throw Exception{full_name + ": dictionary of layout 'cache' cannot have empty update queue of size 0",
                            ErrorCodes::TOO_SMALL_BUFFER_SIZE};

        const bool allow_read_expired_keys =
                config.getBool(layout_prefix + ".cache.allow_read_expired_keys", false);

        const size_t update_queue_push_timeout_milliseconds =
                config.getUInt64(layout_prefix + ".cache.update_queue_push_timeout_milliseconds", 10);
        if (update_queue_push_timeout_milliseconds < 10)
            throw Exception{full_name + ": dictionary of layout 'cache' have too little update_queue_push_timeout",
                            ErrorCodes::BAD_ARGUMENTS};

        const size_t query_wait_timeout_milliseconds =
                config.getUInt64(layout_prefix + ".cache.query_wait_timeout_milliseconds", 60000);

        const size_t max_threads_for_updates =
                config.getUInt64(layout_prefix + ".max_threads_for_updates", 4);
        if (max_threads_for_updates == 0)
            throw Exception{full_name + ": dictionary of layout 'cache' cannot have zero threads for updates.",
                            ErrorCodes::BAD_ARGUMENTS};

        return std::make_unique<CacheDictionary>(
                dict_id,
                dict_struct,
                std::move(source_ptr),
                dict_lifetime,
                strict_max_lifetime_seconds,
                size,
                allow_read_expired_keys,
                max_update_queue_size,
                update_queue_push_timeout_milliseconds,
                query_wait_timeout_milliseconds,
                max_threads_for_updates);
    };
    factory.registerLayout("cache", create_layout, false);
}

void CacheDictionary::updateThreadFunction()
{
    setThreadName("AsyncUpdater");
    while (!finished)
    {
        UpdateUnitPtr popped;
        update_queue.pop(popped);

        if (finished)
            break;

        try
        {
            /// Update a bunch of ids.
            update(popped);

            /// Notify thread about finished updating the bunch of ids
            /// where their own ids were included.
            std::unique_lock<std::mutex> lock(update_mutex);

            popped->is_done = true;
            is_update_finished.notify_all();
        }
        catch (...)
        {
            std::unique_lock<std::mutex> lock(update_mutex);

            popped->current_exception = std::current_exception();
            is_update_finished.notify_all();
        }
    }
}

void CacheDictionary::waitForCurrentUpdateFinish(UpdateUnitPtr & update_unit_ptr) const
{
    std::unique_lock<std::mutex> update_lock(update_mutex);

    bool result = is_update_finished.wait_for(
            update_lock,
            std::chrono::milliseconds(query_wait_timeout_milliseconds),
            [&] { return update_unit_ptr->is_done || update_unit_ptr->current_exception; });

    if (!result)
    {
        throw DB::Exception(ErrorCodes::TIMEOUT_EXCEEDED,
                            "Dictionary {} source seems unavailable, because {}ms timeout exceeded.",
                            getDictionaryID().getNameForLogs(), toString(query_wait_timeout_milliseconds));
    }


    if (update_unit_ptr->current_exception)
    {
        // There might have been a single update unit for multiple callers in
        // independent threads, and current_exception will be the same for them.
        // Don't just rethrow it, because sharing the same exception object
        // between multiple threads can lead to weird effects if they decide to
        // modify it, for example, by adding some error context.
        try
        {
            std::rethrow_exception(update_unit_ptr->current_exception);
        }
        catch (...)
        {
            throw DB::Exception(ErrorCodes::CACHE_DICTIONARY_UPDATE_FAIL,
                "Update failed for dictionary '{}': {}",
                getDictionaryID().getNameForLogs(),
                getCurrentExceptionMessage(true /*with stack trace*/,
                    true /*check embedded stack trace*/));
        }
    }
}

void CacheDictionary::tryPushToUpdateQueueOrThrow(UpdateUnitPtr & update_unit_ptr) const
{
    if (!update_queue.tryPush(update_unit_ptr, update_queue_push_timeout_milliseconds))
        throw DB::Exception(ErrorCodes::CACHE_DICTIONARY_UPDATE_FAIL,
                "Cannot push to internal update queue in dictionary {}. "
                "Timelimit of {} ms. exceeded. Current queue size is {}",
                getDictionaryID().getNameForLogs(), std::to_string(update_queue_push_timeout_milliseconds),
                std::to_string(update_queue.size()));
}


std::vector<CacheDictionary::AttributeValue> CacheDictionary::getAttributeValuesFromBlockAtPosition(const std::vector<const IColumn *> & column_ptrs, size_t position) 
{
    std::vector<AttributeValue> answer;
    answer.reserve(column_ptrs.size());

    for (size_t i = 0; i < column_ptrs.size(); ++i) 
    {
        const auto pure_column = column_ptrs[i];
        
        if (auto column = typeid_cast<const ColumnUInt8 *>(pure_column)) 
        {
            answer.emplace_back(column->getElement(position));
            continue;
        } 
        if (auto column = typeid_cast<const ColumnUInt16 *>(pure_column)) 
        {
            answer.emplace_back(column->getElement(position));
            continue;
        } 
        if (auto column = typeid_cast<const ColumnUInt32 *>(pure_column)) 
        {
            answer.emplace_back(column->getElement(position));
            continue;
        } 
        if (auto column = typeid_cast<const ColumnUInt64 *>(pure_column)) 
        {
            answer.emplace_back(column->getElement(position));
            continue;
        } 
        if (auto column = typeid_cast<const ColumnUInt128 *>(pure_column)) 
        {
            answer.emplace_back(column->getElement(position));
            continue;
        } 
        if (auto column = typeid_cast<const ColumnInt8 *>(pure_column)) 
        {
            answer.emplace_back(column->getElement(position));
            continue;
        } 
        if (auto column = typeid_cast<const ColumnInt16 *>(pure_column)) 
        {
            answer.emplace_back(column->getElement(position));
            continue;
        } 
        if (auto column = typeid_cast<const ColumnInt32 *>(pure_column)) 
        {
            answer.emplace_back(column->getElement(position));
            continue;
        } 
        if (auto column = typeid_cast<const ColumnInt64 *>(pure_column)) 
        {
            answer.emplace_back(column->getElement(position));
            continue;
        } 
        if (auto column = typeid_cast<const ColumnFloat32 *>(pure_column)) 
        {
            answer.emplace_back(column->getElement(position));
            continue;
        } 
        if (auto column = typeid_cast<const ColumnFloat64 *>(pure_column)) 
        {
            answer.emplace_back(column->getElement(position));
            continue;
        }
        if (auto column = typeid_cast<const ColumnDecimal<Decimal32> *>(pure_column)) 
        {
            answer.emplace_back(column->getElement(position));
            continue;
        }
        if (auto column = typeid_cast<const ColumnDecimal<Decimal64> *>(pure_column)) 
        {
            answer.emplace_back(column->getElement(position));
            continue;
        }
        if (auto column = typeid_cast<const ColumnDecimal<Decimal128> *>(pure_column)) 
        {
            answer.emplace_back(column->getElement(position));
            continue;
        }
        if (auto column = typeid_cast<const ColumnString *>(pure_column)) 
        {
            answer.emplace_back(column->getDataAt(position).toString());
            continue;
        }
    }
    return answer;
}

void CacheDictionary::update(UpdateUnitPtr & update_unit_ptr) const
{
    CurrentMetrics::Increment metric_increment{CurrentMetrics::DictCacheRequests};
    ProfileEvents::increment(ProfileEvents::DictCacheKeysRequested, update_unit_ptr->requested_ids.size());

    auto & map_ids = update_unit_ptr->found_ids;

    size_t found_num = 0;

    const auto now = std::chrono::system_clock::now();

    if (now > backoff_end_time.load())
    {
        try
        {
            auto current_source_ptr = getSourceAndUpdateIfNeeded();

            Stopwatch watch;

            BlockInputStreamPtr stream = current_source_ptr->loadIds(update_unit_ptr->requested_ids);
            stream->readPrefix();

            while (true)
            {
                Block block = stream->read();
                if (!block)
                    break;

                const auto * id_column = typeid_cast<const ColumnUInt64 *>(block.safeGetByPosition(0).column.get());
                if (!id_column)
                    throw Exception{ErrorCodes::TYPE_MISMATCH,
                                    "{}: id column has type different from UInt64.", getDictionaryID().getNameForLogs()};

                const auto & ids = id_column->getData();

                /// cache column pointers
                const auto column_ptrs = ext::map<std::vector>(
                        ext::range(0, attributes.size()), [&block] (size_t i) { return block.safeGetByPosition(i + 1).column.get(); });

                found_num += ids.size();

                for (const auto i : ext::range(0, ids.size()))
                {
                    /// Modifying cache with write lock
                    ProfilingScopedWriteRWLock write_lock{rw_lock, ProfileEvents::DictCacheLockWriteNs};

                    const auto id = ids[i];
                    auto it = map_ids.find(id);

                    /// We have some extra keys from source. Won't add them to cache.
                    if (it == map_ids.end())
                        continue;

                    auto all_values = getAttributeValuesFromBlockAtPosition(column_ptrs, i);

                    auto & all_attributes = it->second;
                    all_attributes.found = true;
                    /// Copy attributes here
                    all_attributes.values = all_values;

                    element_count.fetch_add(1, std::memory_order_relaxed);

                    /// Set deadline
                    std::chrono::system_clock::time_point deadline;
                    if (dict_lifetime.min_sec != 0 && dict_lifetime.max_sec != 0)
                    {
                        std::uniform_int_distribution<UInt64> distribution{dict_lifetime.min_sec, dict_lifetime.max_sec};
                        deadline = now + std::chrono::seconds{distribution(rnd_engine);
                    }
                    else
                        deadline = std::chrono::time_point<std::chrono::system_clock>::max();

                    auto mapped_cell = std::make_shared<MappedCell>(std::move(all_values), deadline);

                    cache.set(id, mapped_cell);
                }
            }

            stream->readSuffix();

            /// Lock just for last_exception safety
            ProfilingScopedWriteRWLock write_lock{rw_lock, ProfileEvents::DictCacheLockWriteNs};

            error_count = 0;
            last_exception = std::exception_ptr{};
            backoff_end_time = std::chrono::system_clock::time_point{};

            ProfileEvents::increment(ProfileEvents::DictCacheRequestTimeNs, watch.elapsed());
        }
        catch (...)
        {
            /// Lock just for last_exception safety
            ProfilingScopedWriteRWLock write_lock{rw_lock, ProfileEvents::DictCacheLockWriteNs};
            ++error_count;
            last_exception = std::current_exception();
            backoff_end_time = now + std::chrono::seconds(calculateDurationWithBackoff(rnd_engine, error_count));

            tryLogException(last_exception, log,
                            "Could not update cache dictionary '" + getDictionaryID().getNameForLogs() +
                            "', next update is scheduled at " + ext::to_string(backoff_end_time.load()));
        }
    }

    ProfileEvents::increment(ProfileEvents::DictCacheKeysRequestedMiss, update_unit_ptr->requested_ids.size() - found_num);
    ProfileEvents::increment(ProfileEvents::DictCacheKeysRequestedFound, found_num);
    ProfileEvents::increment(ProfileEvents::DictCacheRequests);
}

}
