# Handling unavailable tapes

This document will explain how to deal with unavailable tapes in the
Lustre/RobinHood/Phobos toolchain. If a tape in Phobos becomes unavailable,
that may mean data won't be accessible from Lustre anymore, so handling it
gracefully is important for both administrators and users.

## Phobos side

On Phobos' side, you can use the `tape lost` command to completely remove a tape
from the system, which will:
 - remove the tape from the database, ensuring Phobos will not use it again
 - remove every extent that were recorded on that tape
 - remove every layout referencing those extents
 - update the corresponding copies to track if they are still readable or not.

During this command, the new status of every copy with extents on the deleted
tape will be printed, allowing you to easily keep track on what is still
readable or what is definitely lost.

## Lustre side

Since Lustre is only concerned about the archived files and not the tape
itself, its side is easier. To handle a lost tape, we only need to declare that
all the objects which cannot be read anymore (obtained during the `tape lost`
command) are HSM lost.

To do so however, you need to get for each object the FID of their associated
entry in the filesystem. Once done, simply use the following command for each
entry:

```
lfs hsm_set LOST <FID>
```

With this, impacted entries will have their HSM copy declared lost. If the entry
itself had not been released before, then its content is still available. If it
had been released however, the entry's content may be lost forever (depending
on the tape's state, but we will talk about this later). For the rest of this
document, we will consider the entry is still available through its regular
filesystem component.

## RobinHood side

For RobinHood, you only need to reflect the new states of the impacted inodes,
meaning you must update information in the database so that the inodes are
registered as having no more HSM component available.

This can be done by using the changelog reader, as the usage `hsm_set` will
have created a corresponding changelog, and so the entry will be updated aswell.

## Continuing operation

At this point, the tape is unavailable, Phobos has registered that information
both with regard to the medium becoming unavailable and the copies being
readable or unreadable, Lustre has marked the entries as having lost their
HSM component, and RobinHood has reflected that change in its database.

Therefore, regular usage of the filesystem can continue, and the RobinHood
policies may continue without any issue. This means that when RobinHood detects
it is time to archive the entries anew, Lustre will do the `hsm_archive`
command, Phobos will receive the request and will put the entry on a new medium,
effectively archiving it.

## What to do with the tape

Now regarding the tape itself, it can be in one of four states:
 - the tape is physically lost
 - the tape is unusable (damaged, destroyed, ...)
 - the tape is corrupt
 - the tape is usable

For the first state and second, Phobos cannot do anything.

For the third state, you can add the tape back to the tape library, and try to
use Phobos to format it again. You can do so with the following command:

```
phobos tape format <tape label>
```

If the command suceeds, then the tape will be inserted back into the system as
a fresh new one, and Phobos may use it in the future to try and hold data.

For the fourth state, the tape can be inserted back into Phobos as an already
filled Phobos tape. However, you must ensure the extents have been properly
deleted using the `tape lost` command as explained in the
(Phobos side)[#Phobos-side] section. Then, use
the following command:

```
phobos tape import <tape label>
```

This command will add the tape into the system, and read its content to register
any extent found. When done reading the content, it will associate any extent
found with their copies and objects, which may mean that copies impacted by
the loss of the tape will be readable/whole again.

## Future features

We currently have 3 features planned related to this procedure to make it
easier:
 - the `extent delete` command to delete an extent from the database and update
the copy and object linked to it, while also showing if the copy and object are
still readable or not
 - the garbage collector to automatically handle `orphan` extents and delete
them from the database.
 - the `tape export` command to remove a tape from the system but handling it
as if it were to come back one day.
