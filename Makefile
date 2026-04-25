CC ?= gcc
AR ?= ar
ARFLAGS ?= rcs

CFLAGS ?= -O2 -g
CFLAGS += -std=gnu99 -Wall -Wextra -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
CFLAGS += -Iuser -I. -DTUX3_BUILD=1 -DTUX3_FLUSHER_SYNC -DTUX3_TRACE=0
CFLAGS += -DTUX3_STANDALONE_QUIET=1
CFLAGS += -DLOCK_DEBUG=1 -DCONFIG_REFCOUNT_FULL=1
CFLAGS += -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
CFLAGS += -Wno-type-limits -Wno-unused-function -Wno-unused-variable

LIBKLIB_OBJS = \
	user/libklib/find_bit.o user/libklib/fs.o user/libklib/fs_types.o \
	user/libklib/list_sort.o user/libklib/mm-util.o user/libklib/parser.o \
	user/libklib/refcount.o user/libklib/seq_file.o user/libklib/slab.o \
	user/libklib/string.o user/libklib/time.o user/libklib/uidgid.o

USER_OBJS = \
	user/commit.o user/current_task.o user/dir.o user/diskio.o \
	user/filemap.o user/inode.o user/namei.o user/options.o \
	user/super.o user/utility.o user/writeback.o user/fault_inject.o

KERN_OBJS = \
	balloc.o btree.o dleaf.o iattr.o ileaf.o log.o orphan.o \
	policy.o replay.o xattr.o

LIBTUX3_OBJS = $(USER_OBJS) $(KERN_OBJS)

.PHONY: all clean
all: mkfs.tux3

libklib.a: $(LIBKLIB_OBJS)
	$(AR) $(ARFLAGS) $@ $^

libtux3.a: $(LIBTUX3_OBJS)
	$(AR) $(ARFLAGS) $@ $^

$(KERN_OBJS): CFLAGS += -include user/tux3user.h

mkfs.tux3: mkfs.tux3.o libtux3.a libklib.a
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f mkfs.tux3 *.o user/*.o user/libklib/*.o libtux3.a libklib.a
