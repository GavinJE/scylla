/*
 * Copyright (C) 2015-present ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "lists.hh"
#include "update_parameters.hh"
#include "column_identifier.hh"
#include "cql3_type.hh"
#include "constants.hh"
#include <boost/iterator/transform_iterator.hpp>
#include "types/list.hh"
#include "utils/UUID_gen.hh"

namespace cql3 {

lw_shared_ptr<column_specification>
lists::index_spec_of(const column_specification& column) {
    return make_lw_shared<column_specification>(column.ks_name, column.cf_name,
            ::make_shared<column_identifier>(format("idx({})", *column.name), true), int32_type);
}

lw_shared_ptr<column_specification>
lists::uuid_index_spec_of(const column_specification& column) {
    return make_lw_shared<column_specification>(column.ks_name, column.cf_name,
            ::make_shared<column_identifier>(format("uuid_idx({})", *column.name), true), uuid_type);
}


lists::value
lists::value::from_serialized(const raw_value_view& val, const list_type_impl& type, cql_serialization_format sf) {
    try {
        utils::chunked_vector<managed_bytes_opt> elements;
        if (sf.collection_format_unchanged()) {
            utils::chunked_vector<managed_bytes> tmp = val.with_value([sf] (const FragmentedView auto& v) {
                return partially_deserialize_listlike(v, sf);
            });
            elements.reserve(tmp.size());
            for (auto&& element : tmp) {
                elements.emplace_back(std::move(element));
            }
        } else [[unlikely]] {
            auto l = val.deserialize<list_type_impl::native_type>(type, sf);
            elements.reserve(l.size());
            for (auto&& element : l) {
                // elements can be null in lists that represent a set of IN values
                elements.push_back(element.is_null() ? managed_bytes_opt() : managed_bytes_opt(type.get_elements_type()->decompose(element)));
            }
        }
        return value(std::move(elements), type.shared_from_this());
    } catch (marshal_exception& e) {
        throw exceptions::invalid_request_exception(e.what());
    }
}

cql3::raw_value
lists::value::get(const query_options& options) {
    return cql3::raw_value::make_value(get_with_protocol_version(cql_serialization_format::internal()));
}

managed_bytes
lists::value::get_with_protocol_version(cql_serialization_format sf) {
    // Can't use boost::indirect_iterator, because optional is not an iterator
    auto deref = [] (managed_bytes_opt& x) { return *x; };
    return collection_type_impl::pack_fragmented(
            boost::make_transform_iterator(_elements.begin(), deref),
            boost::make_transform_iterator( _elements.end(), deref),
            _elements.size(), sf);
}

bool
lists::value::equals(const list_type_impl& lt, const value& v) {
    if (_elements.size() != v._elements.size()) {
        return false;
    }
    return std::equal(_elements.begin(), _elements.end(),
            v._elements.begin(),
            [t = lt.get_elements_type()] (const managed_bytes_opt& e1, const managed_bytes_opt& e2) { return t->equal(*e1, *e2); });
}

sstring
lists::value::to_string() const {
    std::ostringstream os;
    os << "[";
    bool is_first = true;
    for (auto&& e : _elements) {
        if (!is_first) {
            os << ", ";
        }
        is_first = false;
        os << to_hex(e);
    }
    os << "]";
    return os.str();
}

bool
lists::delayed_value::contains_bind_marker() const {
    // False since we don't support them in collection
    return false;
}

void
lists::delayed_value::fill_prepare_context(prepare_context& ctx) const {
}

shared_ptr<terminal>
lists::delayed_value::bind(const query_options& options) {
    utils::chunked_vector<managed_bytes_opt> buffers;
    buffers.reserve(_elements.size());
    for (auto&& t : _elements) {
        auto bo = expr::evaluate_to_raw_view(t, options);

        if (bo.is_null()) {
            throw exceptions::invalid_request_exception("null is not supported inside collections");
        }
        if (bo.is_unset_value()) {
            return constants::UNSET_VALUE;
        }

        buffers.push_back(bo.with_value([] (const FragmentedView auto& v) { return managed_bytes(v); }));
    }
    return ::make_shared<value>(buffers, _my_type);
}

shared_ptr<terminal>
lists::delayed_value::bind_ignore_null(const query_options& options) {
    utils::chunked_vector<managed_bytes_opt> buffers;
    buffers.reserve(_elements.size());
    for (auto&& t : _elements) {
        auto bo = expr::evaluate_to_raw_view(t, options);

        if (bo.is_null()) {
            continue;
        }
        if (bo.is_unset_value()) {
            return constants::UNSET_VALUE;
        }

        buffers.push_back(bo.with_value([] (const FragmentedView auto& v) { return managed_bytes(v); }));
    }
    return ::make_shared<value>(buffers, _my_type);
}

expr::expression lists::delayed_value::to_expression() {
    std::vector<expr::expression> new_elements;
    new_elements.reserve(_elements.size());

    for (shared_ptr<term>& e : _elements) {
        new_elements.emplace_back(expr::to_expression(e));
    }

    return expr::collection_constructor {
        .style = expr::collection_constructor::style_type::list,
        .elements = std::move(new_elements),
        .type = _my_type
    };
}

::shared_ptr<terminal>
lists::marker::bind(const query_options& options) {
    const auto& value = options.get_value_at(_bind_index);
    auto& ltype = dynamic_cast<const list_type_impl&>(_receiver->type->without_reversed());
    if (value.is_null()) {
        return nullptr;
    } else if (value.is_unset_value()) {
        return constants::UNSET_VALUE;
    } else {
        try {
            value.validate(ltype, options.get_cql_serialization_format());
            return make_shared<lists::value>(value::from_serialized(value, ltype, options.get_cql_serialization_format()));
        } catch (marshal_exception& e) {
            throw exceptions::invalid_request_exception(
                    format("Exception while binding column {:s}: {:s}", _receiver->name->to_cql_string(), e.what()));
        }
    }
}

expr::expression lists::marker::to_expression() {
    return expr::bind_variable {
        .shape = expr::bind_variable::shape_type::scalar,
        .bind_index = _bind_index,
        .value_type = _receiver->type
    };
}

void
lists::setter::execute(mutation& m, const clustering_key_prefix& prefix, const update_parameters& params) {
    auto value = expr::evaluate(_t, params._options);
    execute(m, prefix, params, column, std::move(value));
}

void
lists::setter::execute(mutation& m, const clustering_key_prefix& prefix, const update_parameters& params, const column_definition& column, const expr::constant& value) {
    if (value.is_unset_value()) {
        return;
    }
    if (column.type->is_multi_cell()) {
        // Delete all cells first, then append new ones
        collection_mutation_view_description mut;
        mut.tomb = params.make_tombstone_just_before();

        m.set_cell(prefix, column, mut.serialize(*column.type));
    }
    do_append(value, m, prefix, column, params);
}

bool
lists::setter_by_index::requires_read() const {
    return true;
}

void
lists::setter_by_index::fill_prepare_context(prepare_context& ctx) const {
    operation::fill_prepare_context(ctx);
    _idx->fill_prepare_context(ctx);
}

void
lists::setter_by_index::execute(mutation& m, const clustering_key_prefix& prefix, const update_parameters& params) {
    // we should not get here for frozen lists
    assert(column.type->is_multi_cell()); // "Attempted to set an individual element on a frozen list";

    auto index = expr::evaluate_to_raw_view(_idx, params._options);
    if (index.is_null()) {
        throw exceptions::invalid_request_exception("Invalid null value for list index");
    }
    if (index.is_unset_value()) {
        throw exceptions::invalid_request_exception("Invalid unset value for list index");
    }
    auto value = expr::evaluate_to_raw_view(_t, params._options);
    if (value.is_unset_value()) {
        return;
    }

    auto idx = index.deserialize<int32_t>(*int32_type);
    auto&& existing_list_opt = params.get_prefetched_list(m.key(), prefix, column);
    if (!existing_list_opt) {
        throw exceptions::invalid_request_exception("Attempted to set an element on a list which is null");
    }
    auto&& existing_list = *existing_list_opt;
    // we verified that index is an int32_type
    if (idx < 0 || size_t(idx) >= existing_list.size()) {
        throw exceptions::invalid_request_exception(format("List index {:d} out of bound, list has size {:d}",
                idx, existing_list.size()));
    }

    auto ltype = static_cast<const list_type_impl*>(column.type.get());
    const data_value& eidx_dv = existing_list[idx].first;
    bytes eidx = eidx_dv.type()->decompose(eidx_dv);
    collection_mutation_description mut;
    mut.cells.reserve(1);
    if (!value) {
        mut.cells.emplace_back(std::move(eidx), params.make_dead_cell());
    } else {
        mut.cells.emplace_back(std::move(eidx),
                params.make_cell(*ltype->value_comparator(), value, atomic_cell::collection_member::yes));
    }

    m.set_cell(prefix, column, mut.serialize(*ltype));
}

bool
lists::setter_by_uuid::requires_read() const {
    return false;
}

void
lists::setter_by_uuid::execute(mutation& m, const clustering_key_prefix& prefix, const update_parameters& params) {
    // we should not get here for frozen lists
    assert(column.type->is_multi_cell()); // "Attempted to set an individual element on a frozen list";

    auto index = expr::evaluate_to_raw_view(_idx, params._options);
    auto value = expr::evaluate_to_raw_view(_t, params._options);

    if (!index) {
        throw exceptions::invalid_request_exception("Invalid null value for list index");
    }

    auto ltype = static_cast<const list_type_impl*>(column.type.get());

    collection_mutation_description mut;
    mut.cells.reserve(1);

    if (!value) {
        mut.cells.emplace_back(to_bytes(index), params.make_dead_cell());
    } else {
        mut.cells.emplace_back(to_bytes(index), params.make_cell(*ltype->value_comparator(), value, atomic_cell::collection_member::yes));
    }

    m.set_cell(prefix, column, mut.serialize(*ltype));
}

void
lists::appender::execute(mutation& m, const clustering_key_prefix& prefix, const update_parameters& params) {
    const expr::constant value = expr::evaluate(_t, params._options);
    if (value.is_unset_value()) {
        return;
    }
    assert(column.type->is_multi_cell()); // "Attempted to append to a frozen list";
    do_append(value, m, prefix, column, params);
}

void
lists::do_append(const expr::constant& list_value,
        mutation& m,
        const clustering_key_prefix& prefix,
        const column_definition& column,
        const update_parameters& params) {
    if (column.type->is_multi_cell()) {
        // If we append null, do nothing. Note that for Setter, we've
        // already removed the previous value so we're good here too
        if (list_value.is_null_or_unset()) {
            return;
        }

        auto ltype = static_cast<const list_type_impl*>(column.type.get());

        auto&& to_add = expr::get_list_elements(list_value);
        collection_mutation_description appended;
        appended.cells.reserve(to_add.size());
        for (auto&& e : to_add) {
            try {
                auto uuid1 = utils::UUID_gen::get_time_UUID_bytes_from_micros_and_submicros(
                    std::chrono::microseconds{params.timestamp()},
                    params._options.next_list_append_seq());
                auto uuid = bytes(reinterpret_cast<const int8_t*>(uuid1.data()), uuid1.size());
                // FIXME: can e be empty?
                appended.cells.emplace_back(
                    std::move(uuid),
                    params.make_cell(*ltype->value_comparator(), e, atomic_cell::collection_member::yes));
            } catch (utils::timeuuid_submicro_out_of_range&) {
                throw exceptions::invalid_request_exception("Too many list values per single CQL statement or batch");
            }
        }
        m.set_cell(prefix, column, appended.serialize(*ltype));
    } else {
        // for frozen lists, we're overwriting the whole cell value
        if (list_value.is_null()) {
            m.set_cell(prefix, column, params.make_dead_cell());
        } else {
            m.set_cell(prefix, column, params.make_cell(*column.type, list_value.value.to_view()));
        }
    }
}

void
lists::prepender::execute(mutation& m, const clustering_key_prefix& prefix, const update_parameters& params) {
    assert(column.type->is_multi_cell()); // "Attempted to prepend to a frozen list";
    expr::constant lvalue = expr::evaluate(_t, params._options);
    if (lvalue.is_null_or_unset()) {
        return;
    }
    assert(lvalue.type->is_list());

    // For prepend we need to be able to generate a unique but decreasing
    // timeuuid. We achieve that by by using a time in the past which
    // is 2x the distance between the original timestamp (it
    // would be the current timestamp, user supplied timestamp, or
    // unique monotonic LWT timestsamp, whatever is in query
    // options) and a reference time of Jan 1 2010 00:00:00.
    // E.g. if query timestamp is Jan 1 2020 00:00:00, the prepend
    // timestamp will be Jan 1, 2000, 00:00:00.

    // 2010-01-01T00:00:00+00:00 in api::timestamp_time format (microseconds)
    static constexpr int64_t REFERENCE_TIME_MICROS = 1262304000L * 1000 * 1000;

    int64_t micros = params.timestamp();
    if (micros > REFERENCE_TIME_MICROS) {
        micros = REFERENCE_TIME_MICROS - (micros - REFERENCE_TIME_MICROS);
    } else {
        // Scylla, unlike Cassandra, respects user-supplied timestamps
        // in prepend, but there is nothing useful it can do with
        // a timestamp less than Jan 1, 2010, 00:00:00.
        throw exceptions::invalid_request_exception("List prepend custom timestamp must be greater than Jan 1 2010 00:00:00");
    }

    collection_mutation_description mut;
    utils::chunked_vector<managed_bytes> list_elements = expr::get_list_elements(lvalue);
    mut.cells.reserve(list_elements.size());

    auto ltype = static_cast<const list_type_impl*>(column.type.get());
    int clockseq = params._options.next_list_prepend_seq(list_elements.size(), utils::UUID_gen::SUBMICRO_LIMIT);
    for (auto&& v : list_elements) {
        try {
            auto uuid = utils::UUID_gen::get_time_UUID_bytes_from_micros_and_submicros(std::chrono::microseconds{micros}, clockseq++);
            mut.cells.emplace_back(bytes(uuid.data(), uuid.size()), params.make_cell(*ltype->value_comparator(), v, atomic_cell::collection_member::yes));
        } catch (utils::timeuuid_submicro_out_of_range&) {
            throw exceptions::invalid_request_exception("Too many list values per single CQL statement or batch");
        }
    }
    m.set_cell(prefix, column, mut.serialize(*ltype));
}

bool
lists::discarder::requires_read() const {
    return true;
}

void
lists::discarder::execute(mutation& m, const clustering_key_prefix& prefix, const update_parameters& params) {
    assert(column.type->is_multi_cell()); // "Attempted to delete from a frozen list";

    auto&& existing_list = params.get_prefetched_list(m.key(), prefix, column);
    // We want to call bind before possibly returning to reject queries where the value provided is not a list.
    expr::constant lvalue = expr::evaluate(_t, params._options);

    if (!existing_list) {
        return;
    }

    auto&& elist = *existing_list;

    if (elist.empty()) {
        return;
    }

    if (lvalue.is_null_or_unset()) {
        return;
    }

    assert(lvalue.type->is_list());

    auto ltype = static_cast<const list_type_impl*>(column.type.get());

    // Note: below, we will call 'contains' on this toDiscard list for each element of existingList.
    // Meaning that if toDiscard is big, converting it to a HashSet might be more efficient. However,
    // the read-before-write this operation requires limits its usefulness on big lists, so in practice
    // toDiscard will be small and keeping a list will be more efficient.
    auto&& to_discard = expr::get_list_elements(lvalue);
    collection_mutation_description mnew;
    for (auto&& cell : elist) {
        auto has_value = [&] (bytes_view value) {
            return std::find_if(to_discard.begin(), to_discard.end(),
                                [ltype, value] (auto&& v) { return ltype->get_elements_type()->equal(v, value); })
                                         != to_discard.end();
        };
        bytes eidx = cell.first.type()->decompose(cell.first);
        bytes value = cell.second.type()->decompose(cell.second);
        if (has_value(value)) {
            mnew.cells.emplace_back(std::move(eidx), params.make_dead_cell());
        }
    }
    m.set_cell(prefix, column, mnew.serialize(*ltype));
}

bool
lists::discarder_by_index::requires_read() const {
    return true;
}

void
lists::discarder_by_index::execute(mutation& m, const clustering_key_prefix& prefix, const update_parameters& params) {
    assert(column.type->is_multi_cell()); // "Attempted to delete an item by index from a frozen list";
    expr::constant index = expr::evaluate(_t, params._options);
    if (index.is_null()) {
        throw exceptions::invalid_request_exception("Invalid null value for list index");
    }
    if (index.is_unset_value()) {
        return;
    }

    auto&& existing_list_opt = params.get_prefetched_list(m.key(), prefix, column);
    int32_t idx = index.value.to_view().deserialize<int32_t>(*int32_type);

    if (!existing_list_opt) {
        throw exceptions::invalid_request_exception("Attempted to delete an element from a list which is null");
    }
    auto&& existing_list = *existing_list_opt;
    if (idx < 0 || size_t(idx) >= existing_list.size()) {
        throw exceptions::invalid_request_exception(format("List index {:d} out of bound, list has size {:d}", idx, existing_list.size()));
    }
    collection_mutation_description mut;
    const data_value& eidx_dv = existing_list[idx].first;
    bytes eidx = eidx_dv.type()->decompose(eidx_dv);
    mut.cells.emplace_back(std::move(eidx), params.make_dead_cell());
    m.set_cell(prefix, column, mut.serialize(*column.type));
}

}
