# Copyright 2019-present ScyllaDB
#
# This file is part of Scylla.
#
# Scylla is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Scylla is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with Scylla.  If not, see <http://www.gnu.org/licenses/>.

# Various utility functions which are useful for multiple tests

import string
import random
import collections
import time
from contextlib import contextmanager
from botocore.hooks import HierarchicalEmitter

def random_string(length=10, chars=string.ascii_uppercase + string.digits):
    return ''.join(random.choice(chars) for x in range(length))

def random_bytes(length=10):
    return bytearray(random.getrandbits(8) for _ in range(length))

# Utility functions for scan and query into an array of items, reading
# the full (possibly requiring multiple requests to read successive pages).
# For convenience, ConsistentRead=True is used by default, as most tests
# need it to run correctly on a multi-node cluster. Callers who need to
# override it, can (this is necessary in GSI tests, where ConsistentRead=True
# is not supported).
def full_scan(table, ConsistentRead=True, **kwargs):
    response = table.scan(ConsistentRead=ConsistentRead, **kwargs)
    items = response['Items']
    while 'LastEvaluatedKey' in response:
        response = table.scan(ExclusiveStartKey=response['LastEvaluatedKey'],
            ConsistentRead=ConsistentRead, **kwargs)
        items.extend(response['Items'])
    return items

# full_scan_and_count returns both items and count as returned by the server.
# Note that count isn't simply len(items) - the server returns them
# independently. e.g., with Select='COUNT' the items are not returned, but
# count is.
def full_scan_and_count(table, ConsistentRead=True, **kwargs):
    response = table.scan(ConsistentRead=ConsistentRead, **kwargs)
    items = []
    count = 0
    if 'Items' in response:
        items.extend(response['Items'])
    if 'Count' in response:
        count = count + response['Count']
    while 'LastEvaluatedKey' in response:
        response = table.scan(ExclusiveStartKey=response['LastEvaluatedKey'],
            ConsistentRead=ConsistentRead, **kwargs)
        if 'Items' in response:
            items.extend(response['Items'])
        if 'Count' in response:
            count = count + response['Count']
    return (count, items)

# Utility function for fetching the entire results of a query into an array of items
def full_query(table, ConsistentRead=True, **kwargs):
    response = table.query(ConsistentRead=ConsistentRead, **kwargs)
    items = response['Items']
    while 'LastEvaluatedKey' in response:
        response = table.query(ExclusiveStartKey=response['LastEvaluatedKey'],
            ConsistentRead=ConsistentRead, **kwargs)
        items.extend(response['Items'])
    return items

# full_query_and_counts returns both items and counts (pre-filter and
# post-filter count) as returned by the server.
# Note that count isn't simply len(items) - the server returns them
# independently. e.g., with Select='COUNT' the items are not returned, but
# count is.
def full_query_and_counts(table, ConsistentRead=True, **kwargs):
    response = table.query(ConsistentRead=ConsistentRead, **kwargs)
    items = []
    prefilter_count = 0
    postfilter_count = 0
    pages = 0
    if 'Items' in response:
        items.extend(response['Items'])
        pages = pages + 1
    if 'Count' in response:
        postfilter_count = postfilter_count + response['Count']
    if 'ScannedCount' in response:
        prefilter_count = prefilter_count + response['ScannedCount']
    while 'LastEvaluatedKey' in response:
        response = table.query(ExclusiveStartKey=response['LastEvaluatedKey'],
            ConsistentRead=ConsistentRead, **kwargs)
        if 'Items' in response:
            items.extend(response['Items'])
            pages = pages + 1
        if 'Count' in response:
            postfilter_count = postfilter_count + response['Count']
        if 'ScannedCount' in response:
            prefilter_count = prefilter_count + response['ScannedCount']
    return (prefilter_count, postfilter_count, pages, items)

# To compare two lists of items (each is a dict) without regard for order,
# "==" is not good enough because it will fail if the order is different.
# The following function, multiset() converts the list into a multiset
# (set with duplicates) where order doesn't matter, so the multisets can
# be compared.

def freeze(item):
    if isinstance(item, dict):
        return frozenset((key, freeze(value)) for key, value in item.items())
    elif isinstance(item, list):
        return tuple(freeze(value) for value in item)
    return item

def multiset(items):
    return collections.Counter([freeze(item) for item in items])

# NOTE: alternator_Test prefix contains a capital letter on purpose,
#in order to validate case sensitivity in alternator
test_table_prefix = 'alternator_Test_'
def unique_table_name():
    current_ms = int(round(time.time() * 1000))
    # If unique_table_name() is called twice in the same millisecond...
    if unique_table_name.last_ms >= current_ms:
        current_ms = unique_table_name.last_ms + 1
    unique_table_name.last_ms = current_ms
    return test_table_prefix + str(current_ms)
unique_table_name.last_ms = 0

def create_test_table(dynamodb, **kwargs):
    name = unique_table_name()
    print("fixture creating new table {}".format(name))
    table = dynamodb.create_table(TableName=name,
        BillingMode='PAY_PER_REQUEST', **kwargs)
    waiter = table.meta.client.get_waiter('table_exists')
    # recheck every second instead of the default, lower, frequency. This can
    # save a few seconds on AWS with its very slow table creation, but can
    # more on tests on Scylla with its faster table creation turnaround.
    waiter.config.delay = 1
    waiter.config.max_attempts = 200
    waiter.wait(TableName=name)
    return table

# A variant of create_test_table() that can be used in a "with" to
# automatically delete the table when the test ends - as:
#   with new_test_table(dynamodb, ...) as table:
# When possible to share the same table between multiple tests, always
# prefer to use a fixture over using this function directly.
@contextmanager
def new_test_table(dynamodb, **kwargs):
    table = create_test_table(dynamodb, **kwargs)
    try:
        yield table
        # The user's "with" code is running during the yield, so if it
        # throws an exception, we need the cleanup to be in a finally block:
    finally:
        print(f"Deleting table {table.name}")
        table.delete()

# DynamoDB's ListTables request returns up to a single page of table names
# (e.g., up to 100) and it is up to the caller to call it again and again
# to get the next page. This is a utility function which calls it repeatedly
# as much as necessary to get the entire list.
# We deliberately return a list and not a set, because we want the caller
# to be able to recognize bugs in ListTables which causes the same table
# to be returned twice.
def list_tables(dynamodb, limit=100):
    ret = []
    pos = None
    while True:
        if pos:
            page = dynamodb.meta.client.list_tables(Limit=limit, ExclusiveStartTableName=pos);
        else:
            page = dynamodb.meta.client.list_tables(Limit=limit);
        results = page.get('TableNames', None)
        assert(results)
        ret = ret + results
        newpos = page.get('LastEvaluatedTableName', None)
        if not newpos:
            break;
        # It doesn't make sense for Dynamo to tell us we need more pages, but
        # not send anything in *this* page!
        assert len(results) > 0
        assert newpos != pos
        # Note that we only checked that we got back tables, not that we got
        # any new tables not already in ret. So a buggy implementation might
        # still cause an endless loop getting the same tables again and again.
        pos = newpos
    return ret

# Boto3 conveniently transforms native Python types to DynamoDB JSON and back,
# for example one can use the string 'x' as a key and it is transparently
# transformed to the map {'S': 'x'} that Boto3 uses to represent a string.
# While these transformations are very convenient, they prevent us from
# checking various *errors* in the format of API parameters, because boto3
# verifies and/or modifies these parameters for us.
# So the following contextmanager presents a boto3 client which is modifed
# to *not* do these transformations or validations at all.
@contextmanager
def client_no_transform(client):
    # client.meta.events is an "emitter" object listing various hooks, which
    # by default boto3 sets up as explained above. Here we temporarily
    # override it with an empty emitter:
    old_events = client.meta.events
    client.meta.events = HierarchicalEmitter()
    yield client
    client.meta.events = old_events
