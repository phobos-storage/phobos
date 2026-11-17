# Design of Grouped Write I/O scheduler

Definitions:
- *tagset*: the set of tags a tape or write allocation request has;
- *I/O stream*: set of drives used to write a single object. A RAID4 put uses
  an I/O stream of 3 drives. 2 I/O streams mean 2 puts in parallel.
- an I/O stream is said to be *idle* when all its devices can accept more
  requests (current ongoing I/O is strictly lower than the maximum allowed) and
  the devices are ready for scheduling new I/O (as defined by
  `dev_is_sched_ready`)

## Challenges of scheduling writes

### Data locality

The purpose of this scheduler is to group write requests per tagsets, grouping
and layout parameters on the devices. This will enable writes of the same type
to go to the same tapes:

- we want requests with the same grouping to go to the same tapes. Grouping I/O
  by grouping also increases data locality on the medium which can increase read
  speed;
- by grouping tagsets, we avoid incompatibilities between tapes which would
  require swapping tapes before starting an I/O on a new tagset;
- by grouping layout parameters, we make it more likely that repacks and reads
  of several objects will not require too many tape loads. Having a mix of RAID1
  and RAID4 on the same tapes will make repacks more challenging.

By doing these groupings of requests, we reduce the amount of load/unload
operations required to fulfill all the write operations and increase data
locality on each medium.

### Idle drives

Another key aspect of this scheduler is to avoid idle drives. If we don't
schedule some write request due to inefficient load/unload operations, we will
end up having some drives idle depending on the request types and number of
drives. This issue can be mitigated with backfilling algorithms. The first
implementation will very likely not take this into consideration.

### I/O parallelization

There is also a conflict between I/O parallelization and data locality. Some
environments might favor data locality over fast writes. Phobos currently
supports this to some extent though the `fifo_max_write_per_grouping` parameter.

Even for someone wanting maximum parallelism, there is a trade-off between
using more drives for a single grouping/tagset and their size. Writing two small
objects in parallel will offer a very small performance improvement but will
incur a considerably longer time to read those objects.

To mitigate this effect, we propose a configurable size of data under which we
don't run write requests in parallel. For instance, if that number is 10GB, a
batch of 25GB of writes will at most use 3 I/O streams simultaneously regardless
of how many requests are in the batch.

Note that this is not necessarily guarantied. If other drives are busy writing
objects for other groupings/tagsets, only one I/O stream might be available for
this batch.

Since dir can now support multiple concurrent I/O, we should favor running
batches in parallel instead of parallelizing a single batch into multiple I/O
streams. Given 2 batches that can be split into 2 I/O streams and a single dir
which supports 2 I/O in parallel. It is more fair to run a single stream for
each batch than 2 I/O streams for a single batch.

### Resource starvation

Since we can always receive more requests for the same batch, we can end up with
resource starvation. A batch can wait indefinitely to be scheduled. One simple
way to mitigate this issue in the first implementation is to schedule a
different batch when a tape becomes full. We schedule new batches in a round
robin fashion when a tape is full. That way, if we have 10 batches, at worst, a
batch is scheduled every 10 tape swaps.

Since writing a full tape can still take a long time, we might want to consider
adding a size threshold after which a queue is swapped with a new one. To reduce
the overall time requests are waiting, we can also consider scheduling oldest
queues first.

### Hardware error management

The current I/O scheduler API supports retries when a device thread fails to
perform an action. If the error is related to a medium, we can always find a new
one to schedule and retry (at least for tapes). If the error comes from a device
however, it might be difficult to fulfill the current request being scheduled.
If no other device is available, we will stall the other devices when we could
be scheduling different batches instead.

If a sub request cannot be fulfilled at the moment of the retry, the request
should be put back into the queue along with the I/O stream. The other sub
requests which have been fulfilled have to be "unscheduled". We might have
loaded new tapes for nothing. In the next scheduling pass, these tapes might be
reused for a different batch or unloaded if necessary to leave space for a
different batch.

As a result of a failure, some batches might become impossible to schedule.
`io_scheduler_ops::retry()` must not try to find out whether this is true. This
will be done by `io_scheduler_ops::peek_request()` on the next scheduling pass.

In short, `retry()` will only reschedule a new medium on the same device if
possible. Any other situation will result in the request being put back in the
scheduler's queue along with the I/O stream it belongs to.

### Pining I/O streams to devices

The current I/O scheduling API has a big limitation: a request is removed as
soon as it is scheduled. This means that `remove_request()` usually destroys the
queues associated to the request as soon as it is scheduled. If another request
comes in later, the scheduler has forgotten where the request was scheduled.

One way to remove this limitation is to keep the requests around until the next
scheduler loop. Since `peek_request` is always called right after
`remove_request`, we cannot simply keep the requests until the next call to
`peek_request`. We cannot remove them inside `push_request` since we don't
know in advance how many times it will be called when popping requests from the
incoming queue.

A possible solution is to add an epoch number in the `io_sched_handle` struct.
This number will start at 0 during phobosd's startup. It is increased by one on
each iteration. When a request is put in the delayed remove list, the current
epoch is recorded. In `peek_request`, we can remove any request with an epoch
strictly lower than the current epoch.

This behavior can be ported to the `grouped_read` scheduler as well. So the
implementation will be added to the generic `io_scheduler` API. The
`grouped_read` scheduler can later be modified to fix this issue.

## Implementation considerations

### General considerations

As much as possible, all the scheduling decisions must be done in the
`io_scheduler_ops::peek_request()` method. This will avoid the duplication of
scheduling decisions which already exist in the `grouped_read` scheduler.
`io_scheduler_ops::get_device_medium_pair()` will only lookup the drive which is
already associated to the request. If that drive has become unavailable between
the two function calls, the scheduler will return the error `EAGAIN` to indicate
to the upper layer to stop and go back to `peek_request`. The scheduler will
then reevaluate its decisions based on the new state and find the appropriate
next request to schedule.

The current state of the drives must be read from the actual `lrs_dev`
structures to avoid internal bookkeeping in the scheduler. For instance,
counting the number of in-flight I/O for a given grouping must be done by
looping through the devices. To avoid looping through the device list for each
grouping, an intermediary data structure (such as a hashtable) might be useful
but must not be kept to cache information between scheduling calls to avoid
cache inconsistency bugs.

We need to manage requests without tags or groupings in the same manner as the
other requests. This might require customizing some data structures. For
instance, `GHashTable` does not support `NULL` pointers in the default string
based hash functions.

### Internal data structures

The list of members might not be exhaustive. Only the most important aspects are
detailed.

- `struct gw_reqbatch`:
  - `rb_requests`: the list of requests of the same grouping, tagsets and layout
    parameters;
  - `rb_size`: size of the sum of the allocation requests. Avoids looping
    through the list since we might have a big number of requests for a single
    grouping.

- `struct gw_reqset`:
  - `gr_batches`: list of `gw_reqbatch` with the same tagset and layout params
  - `gr_next_rr`: link to the next element to schedule in the `gr_batches` list

- `struct gw_io_stream`:
  - `ios_batch`: reference to the batch of requests. If multiple I/O streams
    share the same batch, they will all point to the same `gw_reqbatch` data
    structure. `peek_request` will free the batch and all the I/O streams when
    the batch is empty. There is no need to keep a reference count to avoid
    double free.
  - `ios_devices`: list of devices used for this batch. Before handing a device
    of this list to the upper layer (in `get_device_medium_pair`), the scheduler
    will check that the device is still in the same state (appropriate tape and
    device state). Some devices can be missing if they where removed by
    `io_scheduler_ops::remove_device()`. The next scheduling pass will try to
    find a replacement for this device.

- `struct gw_state`: global data structure referenced in
  `io_scheduler::private_data`
  - `gws_reqsets`: sets of `gw_reqset` requests with the same tagset and layout
    parameters.
  - `gws_io_streams`: all the I/O streams currently running
  - `gws_config`: configuration information about groupings
    - how many I/O can run in parallel
    - the size threshold over which we can split an I/O stream
  - `gws_delayed_removes`: list of requests removed from the last call to
    `remove_request`. Elements are added in `remove_request` and removed by
    `peek_request`.

### Scheduling algorithm

These are the steps taken by `gw_peek_request` to schedule the writes:

1. find out if any batch has finished using `gws_delayed_removes`. For each
   finished batch, remove the I/O streams;
2. look for batches where a sub request has failed, reschedule it if possible.
   Otherwise, remove it and return an error for all the requests;
3. find out the list of available devices (not in any I/O stream);
4. make sure all current I/O streams have all their devices;
5. find out if we can add more I/O streams. Either by splitting a running
   stream if possible or by starting a new batch (favor new batches);
6. schedule the next request for each I/O stream if possible;
7. return the first available request (round robin over idle I/O streams).

Other methods:

- `push_request`: find the right `gw_reqbatch` and push the request. Create a
  new batch if not available;
- `remove_request`: push the removed request to `gws_delayed_removes`
- `get_device_medium_pair`: find the `gw_io_stream` from which this request was
  taken. Return the device at the index asked by the upper layer if it is in the
  right state for scheduling. Return `EAGAIN` to start a new scheduling
  pass otherwise;
- `retry`: try to find a new medium. If that's not possible, cancel the current
  request and reschedule it later along with its batch;
- `requeue`: called when `EAGAIN` is returned by the scheduler, simply requeue
  the request;
- `add_device`: add the device to the scheduler's device list;
- `remove_device`: simply remove the device from the I/O stream and the device
  list. `peek_request` will manage missing devices from I/O streams;
- `claim_device`:
  - exchange: another scheduler wants one of our devices. Ideally, we don't want
    to redirect the I/O of the current stream to another tape. If that device is
    not currently used for a stream, we can give it away. If it is, we can mark
    the device as needed by someone else, schedule the rest of the current
    grouping and not reuse it for other I/O until it the scheduler asks for it
    again. This behavior is complex since we might have situations where the
    scheduler asking the device might continue on and never ask for it again.
    The first implementation will likely just give the device away to ensure
    correctness.
  - take: the device dispatch algorithm wants to take one of our devices.
    Ideally, we want to give back devices which are idle or between two
    groupings. If that's not possible, give back the device whose current
    grouping has the most amount of data to write. This should result in less
    fragmentation. Another possibility is to never give back a device currently
    used for scheduling a grouping. This will result in temporary repartition
    imbalance. The device will be taken only once the current grouping is
    finished. This needs further investigation to know whether this would lead
    to a correct behavior.

The desired behavior for `retry` needs further investigation as this is a new
behavior that may not be supported yet by the LRS.
