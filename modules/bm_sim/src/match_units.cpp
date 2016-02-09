/* Copyright 2013-present Barefoot Networks, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Antonin Bas (antonin@barefootnetworks.com)
 *
 */

#include <limits>
#include <string>
#include <vector>

#include "bm_sim/match_units.h"
#include "bm_sim/match_tables.h"
#include "bm_sim/logger.h"

namespace bm {

#define HANDLE_VERSION(h) (h >> 32)
#define HANDLE_INTERNAL(h) (h & 0xffffffff)
#define HANDLE_SET(v, i) ((((uint64_t) v) << 32) | i)

using MatchUnit::EntryMeta;

namespace {

size_t nbits_to_nbytes(size_t nbits) {
  return (nbits + 7) / 8;
}

}

// This function is in charge of re-organizing the input key on the fly to
// satisfy the implementation requirements (e.g. LPM matches last...). At the
// same time we keep track of the original order thanks to raw_key_mapping (for
// debugging).
void
MatchKeyBuilder::push_back(KeyF &&input, const ByteContainer &mask) {
  size_t f_nbytes = nbits_to_nbytes(input.nbits);

  auto it = key_input.begin();
  auto end = key_input.end();
  if (input.mtype == MatchKeyParam::Type::VALID) {
    for (; it != end && it->mtype == MatchKeyParam::Type::VALID; it++) continue;
  } else if (input.mtype == MatchKeyParam::Type::LPM) {
    it = key_input.end();
  } else {
    for (; it != end && it->mtype != MatchKeyParam::Type::LPM; it++) continue;
  }
  size_t pos = it - key_input.begin();

  key_input.insert(it, std::move(input));

  assert(raw_key_mapping.size() == raw_key_offsets.size());
  size_t new_offset = nbytes_key;
  for (size_t i = 0; i < raw_key_mapping.size(); i++) {
    if (raw_key_mapping[i] == pos) {
      new_offset = raw_key_offsets[i];
    }
    if (raw_key_mapping[i] >= pos) {
      raw_key_mapping[i]++;
      raw_key_offsets[i] += f_nbytes;
    }
  }
  raw_key_mapping.push_back(pos);
  raw_key_offsets.push_back(new_offset);

  nbytes_key += f_nbytes;

  auto it_big_mask = big_mask.begin();
  for (size_t i = 0; i < pos; i++) {
    it_big_mask += nbits_to_nbytes(key_input[i].nbits);
  }
  big_mask.insert(it_big_mask, mask);
}

void
MatchKeyBuilder::push_back_field(header_id_t header, int field_offset,
                                 size_t nbits, MatchKeyParam::Type mtype) {
  push_back({header, field_offset, mtype, nbits},
            ByteContainer(nbits_to_nbytes(nbits), '\xff'));
}

void
MatchKeyBuilder::push_back_field(header_id_t header, int field_offset,
                                 size_t nbits, const ByteContainer &mask,
                                 MatchKeyParam::Type mtype) {
  assert(mask.size() == nbits_to_nbytes(nbits));
  push_back({header, field_offset, mtype, nbits}, mask);
  has_big_mask = true;
}

void
MatchKeyBuilder::push_back_valid_header(header_id_t header) {
  // set "nbits" to 8 (i.e. 1 byte); it is kind of a hack but ensure that most
  // of the code can be the same...
  push_back({header, 0, MatchKeyParam::Type::VALID, 8},
            ByteContainer(1, '\xff'));
}

void
MatchKeyBuilder::apply_big_mask(ByteContainer *key) const {
  if (has_big_mask)
    key->apply_mask(big_mask);
}

void
MatchKeyBuilder::operator()(const PHV &phv, ByteContainer *key) const {
  for (const auto &in : key_input) {
    const Header &header = phv.get_header(in.header);
    // if speed is an issue here, I can probably come up with something faster
    // (with a switch statement maybe)
    if (in.mtype == MatchKeyParam::Type::VALID) {
      key->push_back(header.is_valid() ? '\x01' : '\x00');
    } else {
      // we do not reset all fields to 0 in between packets
      // so I need this hack if the P4 programmer assumed that:
      // field not valid => field set to 0
      // const Field &field = phv.get_field(p.first, p.second);
      // key->append(field.get_bytes());
      const Field &field = header[in.f_offset];
      if (header.is_valid()) {
        key->append(field.get_bytes());
      } else {
        key->append(std::string(field.get_nbytes(), '\x00'));
      }
    }
  }
  if (has_big_mask)
    key->apply_mask(big_mask);
}

std::string
MatchKeyBuilder::key_to_string(const ByteContainer &key, std::string separator,
                               bool upper_case) const {
  std::ostringstream ret;

  size_t nfields = raw_key_mapping.size();
  bool first = true;
  for (size_t i = 0; i < nfields; i++) {
    const size_t imp_idx = raw_key_mapping.at(i);
    const auto &f_info = key_input.at(imp_idx);
    const size_t byte_offset = raw_key_offsets.at(i);
    if (!first)
      ret << separator;
    ret << key.to_hex(byte_offset, nbits_to_nbytes(f_info.nbits), upper_case);
    first = false;
  }
  return ret.str();
}


MatchErrorCode
MatchUnitAbstract_::get_and_set_handle(internal_handle_t *handle) {
  if (num_entries >= size) {  // table is full
    return MatchErrorCode::TABLE_FULL;
  }

  if (handles.get_handle(handle)) return MatchErrorCode::ERROR;

  num_entries++;
  return MatchErrorCode::SUCCESS;
}

MatchErrorCode
MatchUnitAbstract_::unset_handle(internal_handle_t handle) {
  if (handles.release_handle(handle)) return MatchErrorCode::INVALID_HANDLE;

  num_entries--;
  return MatchErrorCode::SUCCESS;
}

bool
MatchUnitAbstract_::valid_handle_(internal_handle_t handle) const {
  return handles.valid_handle(handle);
}

bool
MatchUnitAbstract_::valid_handle(entry_handle_t handle) const {
  return this->valid_handle_(HANDLE_INTERNAL(handle));
}

EntryMeta &
MatchUnitAbstract_::get_entry_meta(entry_handle_t handle) {
  return this->entry_meta[HANDLE_INTERNAL(handle)];
}

const EntryMeta &
MatchUnitAbstract_::get_entry_meta(entry_handle_t handle) const {
  return this->entry_meta[HANDLE_INTERNAL(handle)];
}

void
MatchUnitAbstract_::reset_counters() {
  // could take a while, but do not block anyone else
  // lock (even read lock) does not have to be held while doing this
  for (EntryMeta &meta : entry_meta) {
    meta.counter.reset_counter();
  }
}

void
MatchUnitAbstract_::set_direct_meters(MeterArray *meter_array) {
  assert(meter_array);
  assert(size == meter_array->size());
  direct_meters = meter_array;
}

Meter &
MatchUnitAbstract_::get_meter(entry_handle_t handle) {
  return direct_meters->at(HANDLE_INTERNAL(handle));
}

MatchErrorCode
MatchUnitAbstract_::set_entry_ttl(entry_handle_t handle, unsigned int ttl_ms) {
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if (!this->valid_handle_(handle_)) return MatchErrorCode::INVALID_HANDLE;
  EntryMeta &meta = entry_meta[handle_];
  meta.timeout_ms = ttl_ms;
  return MatchErrorCode::SUCCESS;
}

void
MatchUnitAbstract_::sweep_entries(std::vector<entry_handle_t> *entries) const {
  using std::chrono::duration_cast;
  using std::chrono::milliseconds;

  auto tp = Packet::clock::now();
  uint64_t now_ms = duration_cast<milliseconds>(tp.time_since_epoch()).count();
  for (auto it = handles.begin(); it != handles.end(); ++it) {
    const EntryMeta &meta = entry_meta[*it];
    assert(now_ms >= meta.ts.get_ms());
    if (meta.timeout_ms > 0 && (now_ms - meta.ts.get_ms() >= meta.timeout_ms)) {
      entries->push_back(HANDLE_SET(meta.version, *it));
    }
  }
}

template<typename V>
typename MatchUnitAbstract<V>::MatchUnitLookup
MatchUnitAbstract<V>::lookup(const Packet &pkt) {
  static thread_local ByteContainer key;
  key.clear();
  build_key(*pkt.get_phv(), &key);

  BMLOG_DEBUG_PKT(pkt, "Looking up key {}", key_to_string(key));

  MatchUnitLookup res = lookup_key(key);
  if (res.found()) {
    EntryMeta &meta = entry_meta[HANDLE_INTERNAL(res.handle)];
    update_counters(&meta.counter, pkt);
    update_ts(&meta.ts, pkt);
  }
  return res;
}

template<typename V>
MatchErrorCode
MatchUnitAbstract<V>::add_entry(const std::vector<MatchKeyParam> &match_key,
                                V value, entry_handle_t *handle, int priority) {
  MatchErrorCode rc = add_entry_(match_key, std::move(value), handle, priority);
  if (rc != MatchErrorCode::SUCCESS) return rc;
  EntryMeta &meta = entry_meta[HANDLE_INTERNAL(*handle)];
  meta.reset();
  meta.version = HANDLE_VERSION(*handle);
  return rc;
}

template<typename V>
MatchErrorCode
MatchUnitAbstract<V>::delete_entry(entry_handle_t handle) {
  return delete_entry_(handle);
}

template<typename V>
MatchErrorCode
MatchUnitAbstract<V>::modify_entry(entry_handle_t handle, V value) {
  return modify_entry_(handle, std::move(value));
}

template<typename V>
MatchErrorCode
MatchUnitAbstract<V>::get_value(entry_handle_t handle, const V **value) {
  return get_value_(handle, value);
}

template<typename V>
void
MatchUnitAbstract<V>::reset_state() {
  this->num_entries = 0;
  this->handles.clear();
  this->entry_meta = std::vector<EntryMeta>(size);
  reset_state_();
}


template<typename V>
typename MatchUnitExact<V>::MatchUnitLookup
MatchUnitExact<V>::lookup_key(const ByteContainer &key) const {
  const auto entry_it = entries_map.find(key);
  // std::cout << "looking up: " << key.to_hex() << "\n";
  if (entry_it == entries_map.end()) return MatchUnitLookup::empty_entry();
  const Entry &entry = entries[entry_it->second];
  entry_handle_t handle = HANDLE_SET(entry.version, entry_it->second);
  return MatchUnitLookup(handle, &entry.value);
}

template<typename V>
MatchErrorCode
MatchUnitExact<V>::add_entry_(const std::vector<MatchKeyParam> &match_key,
                              V value, entry_handle_t *handle, int priority) {
  (void) priority;

  ByteContainer new_key;
  new_key.reserve(this->nbytes_key);

  // take care of valid first

  for (const MatchKeyParam &param : match_key) {
    if (param.type == MatchKeyParam::Type::VALID)
      new_key.append(param.key);
  }

  for (const MatchKeyParam &param : match_key) {
    switch (param.type) {
    case MatchKeyParam::Type::EXACT:
      new_key.append(param.key);
      break;
    case MatchKeyParam::Type::VALID:  // already done
      break;
    default:
      assert(0 && "invalid param type in match_key");
      break;
    }
  }

  if (new_key.size() != this->nbytes_key)
    return MatchErrorCode::BAD_MATCH_KEY;

  // needs to go before duplicate check, because 2 different user keys can
  // become the same key. We would then have a porblem when erasing the key from
  // the hash map.
  // TODO(antonin): maybe change this by modifying delete_entry method
  this->match_key_builder.apply_big_mask(&new_key);

  // check if the key is already present
  if (entries_map.find(new_key) != entries_map.end())
    return MatchErrorCode::DUPLICATE_ENTRY;


  internal_handle_t handle_;
  MatchErrorCode status = this->get_and_set_handle(&handle_);
  if (status != MatchErrorCode::SUCCESS) return status;

  uint32_t version = entries[handle_].version;
  *handle = HANDLE_SET(version, handle_);

  entries_map[new_key] = handle_;  // key is copied, which is not great
  entries[handle_] = Entry(std::move(new_key), std::move(value), version);

  return MatchErrorCode::SUCCESS;
}

template<typename V>
MatchErrorCode
MatchUnitExact<V>::delete_entry_(entry_handle_t handle) {
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if (!this->valid_handle_(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if (HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  entry.version += 1;
  entries_map.erase(entry.key);

  return this->unset_handle(handle_);
}

template<typename V>
MatchErrorCode
MatchUnitExact<V>::modify_entry_(entry_handle_t handle, V value) {
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if (!this->valid_handle_(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if (HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  entry.value = std::move(value);

  return MatchErrorCode::SUCCESS;
}

template<typename V>
MatchErrorCode
MatchUnitExact<V>::get_value_(entry_handle_t handle, const V **value) {
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if (!this->valid_handle(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if (HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  *value = &entry.value;

  return MatchErrorCode::SUCCESS;
}

template<typename V>
void
MatchUnitExact<V>::dump_(std::ostream *stream) const {
  for (internal_handle_t handle_ : this->handles) {
    const Entry &entry = entries[handle_];
    (*stream) << HANDLE_SET(entry.version, handle_) << ": "
              << entry.key.to_hex() << " => ";
    entry.value.dump(stream);
    (*stream) << "\n";
  }
}

template<typename V>
void
MatchUnitExact<V>::reset_state_() {
  entries = std::vector<Entry>(this->size);
  entries_map.clear();
}

template<typename V>
typename MatchUnitLPM<V>::MatchUnitLookup
MatchUnitLPM<V>::lookup_key(const ByteContainer &key) const {
  internal_handle_t handle_;
  if (entries_trie.lookup(key, &handle_)) {
    const Entry &entry = entries[handle_];
    entry_handle_t handle = HANDLE_SET(entry.version, handle_);
    return MatchUnitLookup(handle, &entry.value);
  }
  return MatchUnitLookup::empty_entry();
}

template<typename V>
MatchErrorCode
MatchUnitLPM<V>::add_entry_(const std::vector<MatchKeyParam> &match_key,
                            V value, entry_handle_t *handle, int priority) {
  (void) priority;

  ByteContainer new_key;
  new_key.reserve(this->nbytes_key);
  int prefix_length = 0;
  const MatchKeyParam *lpm_param = nullptr;

  for (const MatchKeyParam &param : match_key) {
    if (param.type == MatchKeyParam::Type::VALID)
      new_key.append(param.key);
  }

  for (const MatchKeyParam &param : match_key) {
    switch (param.type) {
    case MatchKeyParam::Type::EXACT:
      new_key.append(param.key);
      prefix_length += param.key.size() << 3;
      break;
    case MatchKeyParam::Type::LPM:
      assert(!lpm_param && "more than one lpm param in match key");
      lpm_param = &param;
      break;
    case MatchKeyParam::Type::VALID:  // already done
      break;
    default:
      assert(0 && "invalid param type in match_key");
      break;
    }
  }

  assert(lpm_param && "no lpm param in match key");
  new_key.append(lpm_param->key);
  prefix_length += lpm_param->prefix_length;

  if (new_key.size() != this->nbytes_key)
    return MatchErrorCode::BAD_MATCH_KEY;

  // TODO(antonin): does this really make sense for a LPM table?
  this->match_key_builder.apply_big_mask(&new_key);

  // check if the key is already present
  if (entries_trie.has_prefix(new_key, prefix_length))
    return MatchErrorCode::DUPLICATE_ENTRY;

  internal_handle_t handle_;
  MatchErrorCode status = this->get_and_set_handle(&handle_);
  if (status != MatchErrorCode::SUCCESS) return status;

  uint32_t version = entries[handle_].version;
  *handle = HANDLE_SET(version, handle_);

  // key is copied, which is not great
  entries_trie.insert_prefix(new_key, prefix_length, handle_);
  entries[handle_] = Entry(std::move(new_key), prefix_length,
                           std::move(value), version);

  return MatchErrorCode::SUCCESS;
}

template<typename V>
MatchErrorCode
MatchUnitLPM<V>::delete_entry_(entry_handle_t handle) {
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if (!this->valid_handle_(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if (HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  entry.version += 1;
  assert(entries_trie.delete_prefix(entry.key, entry.prefix_length));

  return this->unset_handle(handle_);
}

template<typename V>
MatchErrorCode
MatchUnitLPM<V>::modify_entry_(entry_handle_t handle, V value) {
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if (!this->valid_handle_(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if (HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  entry.value = std::move(value);

  return MatchErrorCode::SUCCESS;
}

template<typename V>
MatchErrorCode
MatchUnitLPM<V>::get_value_(entry_handle_t handle, const V **value) {
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if (!this->valid_handle_(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if (HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  *value = &entry.value;

  return MatchErrorCode::SUCCESS;
}

template<typename V>
void
MatchUnitLPM<V>::dump_(std::ostream *stream) const {
  for (internal_handle_t handle_ : this->handles) {
    const Entry &entry = entries[handle_];
    (*stream) << HANDLE_SET(entry.version, handle_) << ": "
              << entry.key.to_hex() << "/" << entry.prefix_length << " => ";
    entry.value.dump(stream);
    (*stream) << "\n";
  }
}

template<typename V>
void
MatchUnitLPM<V>::reset_state_() {
  entries = std::vector<Entry>(this->size);
  entries_trie.clear();
}


template<typename V>
typename MatchUnitTernary<V>::MatchUnitLookup
MatchUnitTernary<V>::lookup_key(const ByteContainer &key) const {
  int min_priority = std::numeric_limits<int>::max();;
  bool match;

  const Entry *entry;
  const Entry *min_entry = nullptr;
  entry_handle_t min_handle = 0;

  for (auto it = this->handles.begin(); it != this->handles.end(); ++it) {
    entry = &entries[*it];

    if (entry->priority >= min_priority) continue;

    match = true;
    for (size_t byte_index = 0; byte_index < this->nbytes_key; byte_index++) {
      if (entry->key[byte_index] !=
          (key[byte_index] & entry->mask[byte_index])) {
        match = false;
        break;
      }
    }

    if (match) {
      min_priority = entry->priority;
      min_entry = entry;
      min_handle = *it;
    }
  }

  if (min_entry) {
    entry_handle_t handle = HANDLE_SET(min_entry->version, min_handle);
    return MatchUnitLookup(handle, &min_entry->value);
  }

  return MatchUnitLookup::empty_entry();
}

static std::string
create_mask_from_pref_len(int prefix_length, int size) {
  std::string mask(size, '\x00');
  std::fill(mask.begin(), mask.begin() + (prefix_length / 8), '\xff');
  if (prefix_length % 8 != 0) {
    mask[prefix_length / 8] =
      static_cast<char>(0xFF) << (8 - (prefix_length % 8));
  }
  return mask;
}

static void
format_ternary_key(ByteContainer *key, const ByteContainer &mask) {
  assert(key->size() == mask.size());
  for (size_t byte_index = 0; byte_index < mask.size(); byte_index++) {
    (*key)[byte_index] = (*key)[byte_index] & mask[byte_index];
  }
}

template<typename V>
MatchErrorCode
MatchUnitTernary<V>::add_entry_(const std::vector<MatchKeyParam> &match_key,
                                V value, entry_handle_t *handle, int priority) {
  ByteContainer new_key;
  ByteContainer new_mask;
  new_key.reserve(this->nbytes_key);
  new_mask.reserve(this->nbytes_key);

  for (const MatchKeyParam &param : match_key) {
    if (param.type == MatchKeyParam::Type::VALID) {
      new_key.append(param.key);
      new_mask.append("\xff");
    }
  }

  for (const MatchKeyParam &param : match_key) {
    switch (param.type) {
    case MatchKeyParam::Type::EXACT:
      new_key.append(param.key);
      new_mask.append(std::string(param.key.size(), '\xff'));
      break;
    case MatchKeyParam::Type::LPM:
      new_key.append(param.key);
      new_mask.append(create_mask_from_pref_len(param.prefix_length,
                                                param.key.size()));
      break;
    case MatchKeyParam::Type::TERNARY:
      new_key.append(param.key);
      new_mask.append(param.mask);
      break;
    case MatchKeyParam::Type::VALID:  // already done
      break;
    default:
      assert(0 && "invalid param type in match_key");
      break;
    }
  }

  if (new_key.size() != this->nbytes_key)
    return MatchErrorCode::BAD_MATCH_KEY;
  if (new_mask.size() != this->nbytes_key)
    return MatchErrorCode::BAD_MATCH_KEY;

  // in theory I just need to do that for TERNARY MatchKeyParam's...
  format_ternary_key(&new_key, new_mask);

  // TODO(antonin): does this really make sense for a ternary table?
  this->match_key_builder.apply_big_mask(&new_key);

  // check if the key is already present
  if (has_rule(new_key, new_mask, priority))
    return MatchErrorCode::DUPLICATE_ENTRY;

  internal_handle_t handle_;
  MatchErrorCode status = this->get_and_set_handle(&handle_);
  if (status != MatchErrorCode::SUCCESS) return status;

  uint32_t version = entries[handle_].version;
  *handle = HANDLE_SET(version, handle_);

  entries[handle_] = Entry(std::move(new_key), std::move(new_mask), priority,
                           std::move(value), version);

  return MatchErrorCode::SUCCESS;
}

template<typename V>
bool
MatchUnitTernary<V>::has_rule(const ByteContainer &key,
                              const ByteContainer &mask,
                              int priority) const {
  for (auto it = this->handles.begin(); it != this->handles.end(); ++it) {
    const Entry &entry = entries[*it];

    if (entry.priority == priority &&
       entry.key == key &&
       entry.mask == mask) {
      return true;
    }
  }
  return false;
}

template<typename V>
MatchErrorCode
MatchUnitTernary<V>::delete_entry_(entry_handle_t handle) {
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if (!this->valid_handle_(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if (HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  entry.version += 1;

  return this->unset_handle(handle_);
}

template<typename V>
MatchErrorCode
MatchUnitTernary<V>::modify_entry_(entry_handle_t handle, V value) {
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if (!this->valid_handle(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if (HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  entry.value = std::move(value);

  return MatchErrorCode::SUCCESS;
}

template<typename V>
MatchErrorCode
MatchUnitTernary<V>::get_value_(entry_handle_t handle, const V **value) {
  internal_handle_t handle_ = HANDLE_INTERNAL(handle);
  if (!this->valid_handle_(handle_)) return MatchErrorCode::INVALID_HANDLE;
  Entry &entry = entries[handle_];
  if (HANDLE_VERSION(handle) != entry.version)
    return MatchErrorCode::EXPIRED_HANDLE;
  *value = &entry.value;

  return MatchErrorCode::SUCCESS;
}

template<typename V>
void
MatchUnitTernary<V>::dump_(std::ostream *stream) const {
  for (internal_handle_t handle_ : this->handles) {
    const Entry &entry = entries[handle_];
    (*stream) << HANDLE_SET(entry.version, handle_) << ": "
              << entry.key.to_hex() << " &&& " << entry.mask.to_hex() << " => ";
    entry.value.dump(stream);
    (*stream) << "\n";
  }
}

template<typename V>
void
MatchUnitTernary<V>::reset_state_() {
  entries = std::vector<Entry>(this->size);
}


// explicit template instantiation

// I did not think I had to explicitly instantiate MatchUnitAbstract, because it
// is a base class for the others, but I get an linker error if I don't
template class MatchUnitAbstract<MatchTableAbstract::ActionEntry>;
template class MatchUnitAbstract<MatchTableIndirect::IndirectIndex>;

template class MatchUnitExact<MatchTableAbstract::ActionEntry>;
template class MatchUnitExact<MatchTableIndirect::IndirectIndex>;

template class MatchUnitLPM<MatchTableAbstract::ActionEntry>;
template class MatchUnitLPM<MatchTableIndirect::IndirectIndex>;

template class MatchUnitTernary<MatchTableAbstract::ActionEntry>;
template class MatchUnitTernary<MatchTableIndirect::IndirectIndex>;

}  // namespace bm
