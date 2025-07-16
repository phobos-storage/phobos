# Handling unavailable tapes

This document will explain how to deal with unavailable tapes in the
Lustre/RobinHood/Phobos toolchain. If a tape in Phobos becomes unavailable,
that may mean data won't be accessible from Lustre anymore, so handling it
gracefully is important for both administrators and users.

## Phobos side

On Phobos' side, there are 4 steps to declare a tape lost, ensure Phobos will
not use it again, and handle the objects that may not be available anymore.

### First step

For Phobos, a tape becoming unavailable means a medium becomes unavailable, and
thus all the data stored on it cannot be accessed anymore. At this point, the
first thing to do is to prevent Phobos from trying to use that tape, which is
done by using the `lock` command as such:

```
phobos tape lock <tape label>
```

With this, if a client tries to get an object or copy whose content is only
available on that tape will get an error.

### Second step

Next, we must try and prevent access to those objects, which can only be done
by first retrieving the list of extents that are stored on the tape:

```
phobos extent list --output all <tape label>
```

This will show you the complete list of extents stored on that tape, and their
associated copy name and object name.

### Third step

Next you have to determine which objects and copies cannot be accessed anymore.

Note: At the time of writing this document, Phobos has no mechanism to easily
get this list. See the (future features)[#future-features] section.

The only way to do it currently is to, for each object and copy for that
object, determine if they are still readable even though the extent on that
tape has been removed, which depends on the layout. This check can be done using
Phobos' `copy list` and `extent list` commands.

Then, depending on the layout:
 - For RAID1 layouts, if the copy has a replica count superior to 1 and the
other extents for that copy are on available media, the object is still
readable.
 - For RAID4 layouts, if the two other extents for that copy are readable, then
the object is still readable.
 - Otherwise, the copy is considered unreadable. In that case, if the object has
no other readable copy, then the object is also considered unreadable.

When the list of unreadable objects is done, you must delete each one with the
following command:

```
phobos object delete --hard <object ID>
```

This will prevent access to those objects, ensuring no user will try to read
them anymore.

Note: when deleting objects with extents on tape, Phobos will *not* actually
delete the extents from the tape or from the database. For the latter, Phobos
will simply change the extents' state to `orphan` (i.e. not related to any copy
or object) in the database. For the former, the data is expected to be deleted
when running a `repack` operation on that tape.

### Fourth step (optional)

The last step on Phobos' side is optionnal, and is to remove the extents from
the database. At the time of writing this document, Phobos has no mechanism to
delete an extent from the database, so this step must be done manually.

You must first access the PSQL database, and then run the following command:

```
DELETE FROM extent WHERE state = 'orphan' AND medium_id = '<tape label>';
```

At this point, the unavailable tape is locked and cannot be used, and the
objects, copies and extents that are unreadable are no longer referenced in the
database.

## Lustre side

Since Lustre is only concerned about the archived files and not the tape
itself, its side is easier. To handle a lost tape, we only need to declare that
all the objects which cannot be read anymore (obtained during Phobos side's
third step) are HSM lost.

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
both with regard to the medium becoming unavailable and the objects being
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
deleted as explained in Phobos side's (fourth step)[#fourth-step]. Then, use
the following command:

```
phobos tape import <tape label>
```

This command will add the tape into the system, and read its content to register
any extent found. When done reading the content, it will associate any extent
found with their copies and objects, which may mean that objects impacted by
the loss of the tape will be readable/whole again.

## Future features

We currently have 4 features planned related to this procedure to make it
easier:
 - the `extent delete` command to delete an extent from the database and update
the copy and object linked to it, while also showing if the copy and object are
still readable or not (it is planned for version 3.2)
 - the `extent delete --orphan` command to automatically delete extents flagged
as `orphan` in the database (it is planned for version 3.2)
 - the garbage collector to automatically handle `orphan` extents and delete
them from the database.
 - the `tape delete --lost` command to delete a tape from the database and
handle the impacted extents, copies and objects accordingly
 - the `tape export` command to remove a tape from the system but handling it
as if it was to come back one day.
