/*
 * Copyright (C) 2020-present ScyllaDB
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
#pragma once
#include "raft.hh"

namespace raft {

enum class wait_type {
    committed,
    applied
};

// A single uniquely identified participant of a Raft group.
class server {
public:
    struct configuration {
        // automatically snapshot state machine after applying
        // this number of entries
        size_t snapshot_threshold = 1024;
        // how many entries to leave in the log after tacking a snapshot
        size_t snapshot_trailing = 200;
        // max size of appended entries in bytes
        size_t append_request_threshold = 100000;
        // Max number of entries of in-memory part of the log after
        // which requests are stopped to be admitted until the log
        // is shrunk back by a snapshot. Should be greater than
        // whatever the default number of trailing log entries
        // is configured by the snapshot, otherwise the state
        // machine will deadlock on attempt to submit a new entry.
        size_t max_log_size = 5000;
        // If set to true will enable prevoting stage during election
        bool enable_prevoting = true;
    };

    virtual ~server() {}
    // Add command to replicated log
    // Returned future is resolved depending on wait_type parameter:
    //  'committed' - when the entry is committed
    //  'applied'   - when the entry is applied (happens after it is committed)
    // The function has to be called on a leader, throws not_a_leader exception otherwise.
    // May fail because of an internal error or because leader changed and an entry was either
    // replaced by the new leader or the server lost track of it. The former will result in
    // dropped_entry exception the later in commit_status_unknown.
    virtual future<> add_entry(command command, wait_type type) = 0;

    // Set a new cluster configuration. If the configuration is
    // identical to the previous one does nothing.
    // Provided node_info is passed to rpc::add_server() for each
    // new server and rpc::remove_server() is called for each
    // departing server.
    // struct node_info is expected to contain connection
    // information/credentials which is then used by RPC.
    // Can be called on a leader only, otherwise throws not_a_leader.
    // Cannot be called until previous set_configuration() completes
    // otherwise throws conf_change_in_progress exception.
    //
    // Waits until configuration completes, i.e. the server left the joint
    // configuration. The server will apply a dummy entry to
    // make sure this happens.
    //
    // Note: committing a dummy entry extends the opportunity for
    // uncertainty, thus commit_status_unknown exception may be
    // returned even in case of a successful config change.
    virtual future<> set_configuration(server_address_set c_new) = 0;

    // Return the currently known configuration
    virtual raft::configuration get_configuration() const = 0;

    // Load persisted state and start background work that needs
    // to run for this Raft server to function; The object cannot
    // be used until the returned future is resolved.
    virtual future<> start() = 0;

    // Stop this Raft server, all submitted but not completed
    // operations will get an error and callers will not be able
    // to know if they succeeded or not. If this server was
    // a leader it will relinquish its leadership and cease
    // replication.
    virtual future<> abort() = 0;

    // Return Raft protocol current term.
    virtual term_t get_current_term() const = 0;

    // May be called before attempting a read from the local state
    // machine. The read should proceed only after the returned
    // future has resolved successfully.
    virtual future<> read_barrier() = 0;

    // Initiate leader stepdown process.
    // If the node is not a leader returns not_a_leader exception.
    // In case of a timeout returns timeout_error.
    virtual future<> stepdown(logical_clock::duration timeout) = 0;

    // Ad hoc functions for testing
    virtual void wait_until_candidate() = 0;
    virtual future<> wait_election_done() = 0;
    virtual future<> wait_log_idx_term(std::pair<index_t, term_t> idx_log) = 0;
    virtual std::pair<index_t, term_t> log_last_idx_term() = 0;
    virtual void elapse_election() = 0;
    virtual bool is_leader() = 0;
    virtual void tick() = 0;
};

std::unique_ptr<server> create_server(server_id uuid, std::unique_ptr<rpc> rpc,
        std::unique_ptr<state_machine> state_machine, std::unique_ptr<persistence> persistence,
        seastar::shared_ptr<failure_detector> failure_detector, server::configuration config);

} // namespace raft

