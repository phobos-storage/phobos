% Locate feature

# Overview

This document describes the locate feature, to give users a way to locate on
which node an object is currently reachable, based on its current medium
location, and to ensure this location for future get calls.

---

## Use cases
> We consider here a distributed phobos storage system

1. Object location

> User wants to know where an object should be accessed from so that future
> access to this resource is likely to be at this node.

2. Object location on get error

> User wants to know where an object should be accessed from if not available on
> the current phobos node.

3. Medium location

> Admin wants to know from which node a medium is currently reachable to be able
> to access it.

---

## Feature implementation

### API calls
This feature implies some modifications on the Phobos API.

#### Object location
The `phobos_locate()` call retrieve a node name which can be used to access an
object.

```c
/**
 * Retrieve one node name from which an object can be accessed.
 *
 * This function returns the most convenient node to get an object.

 * If possible, this function locks to the returned node the minimum adequate
 * number of media storing extents of this object to ensure that the returned
 * node will be able to get this object. The number of newly added locks is also
 * returned to allow the caller to keep up to date the load of each host, by
 * counting the media that are newly locked to the returned hostname.
 *
 * Among the most convenient nodes, this function will favour the \p focus_host.
 *
 * At least one of \p oid or \p uuid must not be NULL.
 *
 * If \p version is not provided (zero as input), the latest one is located.
 *
 * If \p uuid is not provided, we first try to find the corresponding \p oid
 * from living objects into the object table. If there is no living object with
 * \p oid, we check amongst all deprecated objects. If there is only one
 * corresponding \p uuid, in the deprecated objects, we take this one. If there
 * is more than one \p uuid corresponding to this \p oid, we return -EINVAL.
 *
 * @param[in]   oid         OID of the object to locate (ignored if NULL and
 *                          \p uuid must not be NULL)
 * @param[in]   uuid        UUID of the object to locate (ignored if NULL and
 *                          \p oid must not be NULL)
 * @param[in]   version     Version of the object to locate (ignored if zero)
 * @param[in]   focus_host  Hostname on which the caller would like to access
 *                          the object if there is no more convenient node (if
 *                          NULL, focus_host is set to local hostname)
 * @param[out]  hostname    Allocated and returned hostname of the most
 *                          convenient node on which the object can be accessed
 *                          (NULL is returned on error)
 * @param[out]  nb_new_lock Number of new lock on media added for the returned
 *                          hostname
 *
 * @return                  0 on success or -errno on failure,
 *                          -ENOENT if no object corresponds to input
 *                          -EINVAL if more than one object corresponds to input
 *                          -EAGAIN if there is not any convenient node to
 *                          currently retrieve this object
 *                          -ENODEV if there is no existing medium to retrieve
 *                          this object
 *                          -EADDRNOTAVAIL if we cannot get self hostname
 */
int phobos_locate(const char *obj_id, const char *uuid, int version,
                  const char *focus_host, char **hostname, int *nb_new_lock);
```

The call takes an object ID as input and gives back a node name as output. If
any node can access the object (i.e. no extent of the object is on a currently
locked medium), the call returns the focus_host and takes locks for this host to
ensure the availability of the object in the future. The number of locks that
are newly taken during the call for the output hostname is also returned. This
number can help the caller to evaluate the number of new media that this host
would have to manage.

#### Object retrieval
The `phobos_get()` call remains the same, but it supports a new flag in the
`pho_xfer_desc` data structure called `PHO_XFER_OBJ_BEST_HOST`. If this flag is
set, we will first call `phobos_locate()` on each object with current host as
focus_host (ie: focus_host is set to NULL). We get the object only if it is
located on the current host.

If the `phobos_locate()` returns a hostname which differs from the current host,
the return code of this xfer will be set to -EREMOTE, and a field added to the
XFer parameters, called `node_name`, will be filled with the name of the node
to access to make the `phobos_get()` call.

```c
struct pho_xfer_get_params {
    char *node_name;                    /**< Node name [out] */
};

union pho_xfer_params {
    struct pho_xfer_put_params put;
    struct pho_xfer_get_params get;     /**< GET parameters */
};
```

#### Medium location
The `phobos_admin_medium_locate()` call retrieves the name of the node which
detains the medium, for example where it is mounted (if tape).

```c
/**
 * Retrieve the name of the node which holds a medium or NULL if any node can
 * access this media.
 *
 * @param[in]   adm         Admin module handler.
 * @param[in]   medium_id   ID of the medium to locate.
 * @param[out]  node_name   Name of the node which holds \p medium_id.
 * @return                  0 on success,
 *                         -errno on failure.
 */
int phobos_admin_medium_locate(struct admin_handle *adm,
                               const struct pho_id *medium_id,
                               char **node_name);
```

The call takes a medium ID as input and gives back a node name as output. The
retrieved node name is the one targeted by the locate command if the medium is
not detained by another one and reachable by the local node. In the other cases,
it responds with the node which detains the medium or the first one which can
access it.

### CLI calls
'locate' is an action keyword on objects and media, to retrieve the node names
from where we can access them.

```
$ phobos tape locate tape_id
$ phobos dir locate dir_id
```

As for the other object actions, the 'locate' action does not need the 'object'
keyword.

```
$ phobos locate obj_id
```

To target an object, the 'locate' action can also take two optional arguments
'--uuid' and '--version', allowing one to locate a deprecated object (see
deletion_and_versionning.md for more information on their use).

We can add a preferred hostname to the 'locate' action by using the --focus-host
optional argument. If not set, the focus-host is set to the current hostname.

If possible, the 'locate' action would lock the minimum but adequate number of
media to ensure that the returned hostname will be able to access the targeted
object.

```
$ phobos locate --uid obj_uuid --version obj_vers obj_id --focus-host hostname
```

The command 'phobos get' is given a new option to indicate that the get should
only happen if the current host is the most optimal to retrieve the object.

```
$ phobos get [--best-host] obj_id file
```

---

## Responses to use cases
The following section describes CLI calls utilization or internals following the
use cases presented in the first section.

1. Object location

```sh
$ phobos locate obj_foo
Object 'obj_foo' is accessible on node 'st_node_2'
```

2. Object location on get error

```sh
$ phobos get --locate obj_foo foo
ERROR: Object 'obj_foo' can not be retrieved (object is remote)
Object 'obj_foo' is accessible on node 'st_node_2'
$ ssh st_node_2
$ phobos get obj_foo foo
Object 'obj_foo' succesfully retrieved
```

3. Medium location

```sh
$ phobos tape locate P00000L5
Medium 'P00000L5' is accessible on node 'st_node_2'
$ ssh st_node_2
$ phobos tape format P00000L5
Medium 'P00000L5' is now formatted
```

