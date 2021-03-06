/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2010 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

/* Modified on 2015-09-26 from:
   http://www.opensource.apple.com/source/dyld/dyld-353.2.1/src/
      threadLocalVariables.c

   TLV support mostly exists in dyld but it is not included in iOS runtime.
   Here we take what Apple already developed and enable it.

   Function names considered to be part of the dyld private API have been
   changed by adding the prefix ios, so "dyld_" becomes "iosdyld_". Static
   scope names (functions, vars) and types are left as-is.

   Works with modified LLVM at: https://github.com/smolt/llvm.  TLV
   descriptors are emitted in __iostlv sections instead of usual __thread_vars
   section.  TLS lookup is done via __tls_get_addr
*/

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <pthread.h>
#include <Block.h>
#include <malloc/malloc.h>
#include <mach-o/loader.h>
#include <libkern/OSAtomic.h>

#include "dyld_priv.h"


#if __LP64__
	typedef struct mach_header_64		macho_header;
	#define LC_SEGMENT_COMMAND			LC_SEGMENT_64
	typedef struct segment_command_64	macho_segment_command;
	typedef struct section_64			macho_section;
#else
	typedef struct mach_header			macho_header;
	#define LC_SEGMENT_COMMAND			LC_SEGMENT
	typedef struct segment_command		macho_segment_command;
	typedef struct section				macho_section;
#endif

#ifndef S_THREAD_LOCAL_REGULAR
#define S_THREAD_LOCAL_REGULAR                   0x11
#endif

#ifndef S_THREAD_LOCAL_ZEROFILL
#define S_THREAD_LOCAL_ZEROFILL                  0x12
#endif

#ifndef S_THREAD_LOCAL_VARIABLES
#define S_THREAD_LOCAL_VARIABLES                 0x13
#endif

#ifndef S_THREAD_LOCAL_VARIABLE_POINTERS
#define S_THREAD_LOCAL_VARIABLE_POINTERS         0x14
#endif

#ifndef S_THREAD_LOCAL_INIT_FUNCTION_POINTERS
#define S_THREAD_LOCAL_INIT_FUNCTION_POINTERS    0x15
#endif

#ifndef MH_HAS_TLV_DESCRIPTORS
	#define MH_HAS_TLV_DESCRIPTORS 0x800000
#endif


typedef void (*TermFunc)(void*);


typedef struct TLVHandler {
	struct TLVHandler *next;
	dyld_tlv_state_change_handler handler;
	enum dyld_tlv_states state;
} TLVHandler;

// lock-free prepend-only linked list
static TLVHandler * volatile tlv_handlers = NULL;


struct TLVDescriptor
{
	void*			(*thunk)(struct TLVDescriptor*);
	unsigned long	key;
	unsigned long	offset;
};
typedef struct TLVDescriptor  TLVDescriptor;


// implemented in assembly
//extern void* tlv_get_addr(TLVDescriptor*);
// but this is implemented in this file...
extern void* __tls_get_addr(TLVDescriptor*);

struct TLVImageInfo
{
	pthread_key_t				key;
	const struct mach_header*	mh;
};
typedef struct TLVImageInfo		TLVImageInfo;

static TLVImageInfo*	tlv_live_images = NULL;
static unsigned int		tlv_live_image_alloc_count = 0;
static unsigned int		tlv_live_image_used_count = 0;
static pthread_mutex_t	tlv_live_image_lock = PTHREAD_MUTEX_INITIALIZER;

static void tlv_set_key_for_image(const struct mach_header* mh, pthread_key_t key)
{
	pthread_mutex_lock(&tlv_live_image_lock);
		if ( tlv_live_image_used_count == tlv_live_image_alloc_count ) {
			unsigned int newCount = (tlv_live_images == NULL) ? 8 : 2*tlv_live_image_alloc_count;
			struct TLVImageInfo* newBuffer = malloc(sizeof(TLVImageInfo)*newCount);
			if ( tlv_live_images != NULL ) {
				memcpy(newBuffer, tlv_live_images, sizeof(TLVImageInfo)*tlv_live_image_used_count);
				free(tlv_live_images);
			}
			tlv_live_images = newBuffer;
			tlv_live_image_alloc_count = newCount;
		}
		tlv_live_images[tlv_live_image_used_count].key = key;
		tlv_live_images[tlv_live_image_used_count].mh = mh;
		++tlv_live_image_used_count;
	pthread_mutex_unlock(&tlv_live_image_lock);
}

static const struct mach_header* tlv_get_image_for_key(pthread_key_t key)
{
	const struct mach_header* result = NULL;
	pthread_mutex_lock(&tlv_live_image_lock);
		for(unsigned int i=0; i < tlv_live_image_used_count; ++i) {
			if ( tlv_live_images[i].key == key ) {
				result = tlv_live_images[i].mh;
				break;
			}
		}
	pthread_mutex_unlock(&tlv_live_image_lock);
	return result;
}


static void
tlv_notify(enum dyld_tlv_states state, void *buffer)
{
	if (!tlv_handlers) return;

	// Always use malloc_size() to ensure allocated and deallocated states 
	// send the same size. tlv_free() doesn't have anything else recorded.
	dyld_tlv_info info = { sizeof(info), buffer, malloc_size(buffer) };
	
	for (TLVHandler *h = tlv_handlers; h != NULL; h = h->next) {
		if (h->state == state  &&  h->handler) {
			h->handler(h->state, &info);
		}
	}
}


// called lazily when TLV is first accessed
//__attribute__((visibility("hidden")))
// codegen is better for case for __tls_get_addr if not inlined
__attribute__((noinline))
static void* tlv_allocate_and_initialize_for_key(pthread_key_t key)
{
	const struct mach_header* mh = tlv_get_image_for_key(key);
	if ( mh == NULL )
		return NULL;	// if data structures are screwed up, don't crash
	
	// first pass, find size and template
	uint8_t*		start = NULL;
	unsigned long	size = 0;
	intptr_t		slide = 0;
	bool			slideComputed = false;
	bool			hasInitializers = false;
	const uint32_t	cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)(((uint8_t*)mh) + sizeof(macho_header));
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd == LC_SEGMENT_COMMAND) {
			const macho_segment_command* seg = (macho_segment_command*)cmd;
			if ( !slideComputed && (seg->filesize != 0) ) {
				slide = (uintptr_t)mh - seg->vmaddr;
				slideComputed = true;
			}
			const macho_section* const sectionsStart = (macho_section*)((char*)seg + sizeof(macho_segment_command));
			const macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
			for (const macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
				switch ( sect->flags & SECTION_TYPE ) {
					case S_THREAD_LOCAL_INIT_FUNCTION_POINTERS:
						hasInitializers = true;
						break;
					case S_THREAD_LOCAL_ZEROFILL:
					case S_THREAD_LOCAL_REGULAR:
						if ( start == NULL ) {
							// first of N contiguous TLV template sections, record as if this was only section
							start = (uint8_t*)(sect->addr + slide);
							size = sect->size;
						}
						else {
							// non-first of N contiguous TLV template sections, accumlate values
							const uint8_t* newEnd = (uint8_t*)(sect->addr + slide + sect->size);
							size = newEnd - start;
						}
						break;
				}
			}
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	
	// allocate buffer and fill with template
	void* buffer = malloc(size);
	memcpy(buffer, start, size);
	
	// set this thread's value for key to be the new buffer.
	pthread_setspecific(key, buffer);

	// send tlv state notifications
	tlv_notify(dyld_tlv_state_allocated, buffer);
	
	// second pass, run initializers
	if ( hasInitializers ) {
		cmd = cmds;
		for (uint32_t i = 0; i < cmd_count; ++i) {
			if ( cmd->cmd == LC_SEGMENT_COMMAND) {
				const macho_segment_command* seg = (macho_segment_command*)cmd;
				const macho_section* const sectionsStart = (macho_section*)((char*)seg + sizeof(macho_segment_command));
				const macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
				for (const macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
					if ( (sect->flags & SECTION_TYPE) == S_THREAD_LOCAL_INIT_FUNCTION_POINTERS ) {
						typedef void (*InitFunc)(void);
						InitFunc* funcs = (InitFunc*)(sect->addr + slide);
						const size_t count = sect->size / sizeof(uintptr_t);
						for (size_t i=count; i > 0; --i) {
							InitFunc func = funcs[i-1];
							func();
						}
					}
				}
			}
			cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
		}
	}
	return buffer;
}


// pthread destructor for TLV storage
static void
tlv_free(void *storage)
{
	tlv_notify(dyld_tlv_state_deallocated, storage);
	free(storage);
}

// find start of TLV template
static void* findTlvStart(const macho_section* sectionsStart,
						  const macho_section* sectionsEnd,
						  intptr_t slide)
{
	for (const macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
		if ( (sect->flags & SECTION_TYPE) == S_THREAD_LOCAL_ZEROFILL ||
			 (sect->flags & SECTION_TYPE) == S_THREAD_LOCAL_REGULAR )  {
			return (void*)(sect->addr + slide);
		}
	}
	return NULL;
}

// called when image is loaded
static void tlv_initialize_descriptors(const struct mach_header* mh)
{
	// record start of TLS template. used to calc offset in TLV descriptors
	// when not in a standard thread_local_variables type section
	void*			tlvstart = NULL;
	pthread_key_t	key = 0;
	intptr_t		slide = 0;
	bool			slideComputed = false;
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)(((uint8_t*)mh) + sizeof(macho_header));
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd == LC_SEGMENT_COMMAND) {
			const macho_segment_command* seg = (macho_segment_command*)cmd;
			if ( !slideComputed && (seg->filesize != 0) ) {
				slide = (uintptr_t)mh - seg->vmaddr;
				slideComputed = true;
			}
			const macho_section* const sectionsStart = (macho_section*)((char*)seg + sizeof(macho_segment_command));
			const macho_section* const sectionsEnd = &sectionsStart[seg->nsects];

			// first phase, find start of TLV template sections so can calc
			// offset.	This is only needed for our custom __iostlv section.
			// A S_THREAD_LOCAL_VARIABLES section would already have the
			// correct offset calculated by ld.
			if (!tlvstart) {
				tlvstart = findTlvStart(sectionsStart, sectionsEnd, slide);
			}

			// Are there thread locals?	 If so, search for and intialize TLV
			// descriptors in either __iostlv (custom section for TLV
			// descriptors) or S_THREAD_LOCAL_REGULAR (the official way but
			// disallowed by ld for iOS).
			if (tlvstart) {
				for (const macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
					bool isTLVSection = (sect->flags & SECTION_TYPE) == S_THREAD_LOCAL_VARIABLES;

					if ( isTLVSection || strcmp("__iostlv", sect->sectname) == 0 ) {
						if ( sect->size != 0 ) {
							// allocate pthread key when we first discover this image has TLVs
							if ( key == 0 ) {
								int result = pthread_key_create(&key, &tlv_free);
								if ( result != 0 )
									abort();
								tlv_set_key_for_image(mh, key);
							}
							// initialize each descriptor
							TLVDescriptor* start = (TLVDescriptor*)(sect->addr + slide);
							TLVDescriptor* end = (TLVDescriptor*)(sect->addr + sect->size + slide);
							for (TLVDescriptor* d=start; d < end; ++d) {
								d->thunk = __tls_get_addr;
								d->key = key;
								//d->offset = d->offset;  // offset unchanged
								if (!isTLVSection) {
									d->offset -= (unsigned long)tlvstart;
								}
							}
						}
					}
				}
			}
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
}

// not used but kept for documentation
#if 0
// called by dyld when a image is loaded
static const char* tlv_load_notification(enum dyld_image_states state, uint32_t infoCount, const struct dyld_image_info info[])
{
	// this is called on all images, even those without TLVs, so we want
	// this to be fast.  The linker sets MH_HAS_TLV_DESCRIPTORS so we don't
	// have to search images just to find the don't have TLVs.
	for (uint32_t i=0; i < infoCount; ++i) {
		if ( info[i].imageLoadAddress->flags & MH_HAS_TLV_DESCRIPTORS )
			tlv_initialize_descriptors(info[i].imageLoadAddress);
	}
	return NULL;
}
#endif

// Above is fed to dyld_register_image_state_change_handler(), which is a
// private API.  Something similar that can can be used with public API
// _dyld_register_func_for_add_image() follows:

static void init_tlv_on_add_image(const struct mach_header* mh, intptr_t vmaddr_slide)
{
	// this is called on all images, even those without TLVs.  The linker
	// would normally set
	// MH_HAS_TLV_DESCRIPTORS for sections with TLV descriptors, but not for
	// our custom __iostlv section, so must search all :-(
	//if (mh->flags & MH_HAS_TLV_DESCRIPTORS)
	tlv_initialize_descriptors(mh);
}

// private API name changed

void iosdyld_register_tlv_state_change_handler(enum dyld_tlv_states state, dyld_tlv_state_change_handler handler)
{
	TLVHandler *h = malloc(sizeof(TLVHandler));
	h->state = state;
	h->handler = Block_copy(handler);

	TLVHandler *old;
	do {
		old = tlv_handlers;
		h->next = old;
	} while (! OSAtomicCompareAndSwapPtrBarrier(old, h, (void * volatile *)&tlv_handlers));
}

// private API name changed

void iosdyld_enumerate_tlv_storage(dyld_tlv_state_change_handler handler)
{
	pthread_mutex_lock(&tlv_live_image_lock);
		unsigned int count = tlv_live_image_used_count;
		void *list[count];
		for (unsigned int i = 0; i < count; ++i) {
			list[i] = pthread_getspecific(tlv_live_images[i].key);
		}
	pthread_mutex_unlock(&tlv_live_image_lock);

	for (unsigned int i = 0; i < count; ++i) {
		if (list[i]) {
			dyld_tlv_info info = { sizeof(info), list[i], malloc_size(list[i]) };
			handler(dyld_tlv_state_allocated, &info);
		}
	}
}


//
//  thread_local terminators
//
// C++ 0x allows thread_local C++ objects which have constructors run
// on the thread before any use of the object and the object's destructor
// is run on the thread when the thread terminates.
//
// To support this libdyld gets a pthread key early in process start up and
// uses tlv_finalize and the key's destructor function.  This key must be
// allocated before any thread local variables are instantiated because when
// a thread is terminated, the pthread package runs the destructor function
// on each key's storage values in key allocation order.  Since we want
// C++ objects to be destructred before they are deallocated, we need the 
// destructor key to come before the deallocation key.
//

struct TLVTerminatorListEntry
{
    TermFunc    termFunc;
    void*       objAddr;
};

struct TLVTerminatorList
{
    uint32_t                        allocCount;
    uint32_t                        useCount;
    struct TLVTerminatorListEntry   entries[1];  // variable length
};


static pthread_key_t tlv_terminators_key = 0;

static // not currently used
void _tlv_atexit(TermFunc func, void* objAddr)
{
    // NOTE: this does not need locks because it only operates on current thread data
	struct TLVTerminatorList* list = (struct TLVTerminatorList*)pthread_getspecific(tlv_terminators_key);
    if ( list == NULL ) {
        // handle first allocation
        list = (struct TLVTerminatorList*)malloc(offsetof(struct TLVTerminatorList, entries[1]));
        list->allocCount = 1;
        list->useCount = 1;
        list->entries[0].termFunc = func;
        list->entries[0].objAddr = objAddr;
        pthread_setspecific(tlv_terminators_key, list);
    }
    else {
        if ( list->allocCount == list->allocCount ) {
            // handle resizing allocation 
            uint32_t newAllocCount = list->allocCount * 2;
            size_t newAllocSize = offsetof(struct TLVTerminatorList, entries[newAllocCount]);
            struct TLVTerminatorList* newlist = (struct TLVTerminatorList*)malloc(newAllocSize);
            newlist->allocCount = newAllocCount;
            newlist->useCount = list->useCount;
            for(uint32_t i=0; i < list->useCount; ++i)
                newlist->entries[i] = list->entries[i];
            pthread_setspecific(tlv_terminators_key, newlist);
            free(list);
            list = newlist;
        }
        // handle appending new entry
        list->entries[list->useCount].termFunc = func;
        list->entries[list->useCount].objAddr = objAddr;
        list->useCount += 1;
    }
}

// called by pthreads when the current thread is going away and 
// _tlv_atexit() has been called on the thread.
static void tlv_finalize(void* storage)
{
    struct TLVTerminatorList* list = (struct TLVTerminatorList*)storage;
    // destroy in reverse order of construction
    for(uint32_t i=list->useCount; i > 0 ; --i) {
        struct TLVTerminatorListEntry* entry = &list->entries[i-1];
        if ( entry->termFunc != NULL ) {
            (*entry->termFunc)(entry->objAddr);
        }
    }
    free(storage);
}

// <rdar://problem/13741816>
// called by exit() before it calls cxa_finalize() so that thread_local
// objects are destroyed before global objects.
static // not currently used
void _tlv_exit()
{
	void* termFuncs = pthread_getspecific(tlv_terminators_key);
	if ( termFuncs != NULL )
		tlv_finalize(termFuncs);
}

static
void tlv_initializer()
{
    // create pthread key to handle thread_local destructors
    // NOTE: this key must be allocated before any keys for TLV
    // so that _pthread_tsd_cleanup will run destructors before deallocation
    (void)pthread_key_create(&tlv_terminators_key, &tlv_finalize);
       
    // register with dyld for notification when images are loaded
    //dyld_register_image_state_change_handler(dyld_image_state_bound, true, tlv_load_notification);

    // dyld_register_image_state_change_handler is private API, so change to
    // something public.  Difference is that this has risk that "this" image
    // will be unloaded if we are in shared lib, making init_tlv_on_add_image
    // be invalid.  Currently not a worry on iOS where static user libs are
    // more common
    _dyld_register_func_for_add_image(&init_tlv_on_add_image);
}

// linked images with TLV have references to this symbol, but it is never used at runtime
void _tlv_bootstrap()
{
	abort();
}

// Somehow need to initialize since dyld for iOS 32-bit won't call us.  Was
// not sure of the best place to get this in the init chain, but this seems to
// work for now.
__attribute__((constructor))
static void inittlv()
{
    tlv_initializer();
}

// LLVM https://github.com/smolt/llvm has TLS enabled for iOS
// (arm-darwin/macho and i386-ios) that works by emitting calls to
// __tls_get_addr for thread locals.  It would be nice to change it someday to
// emmit direct calls to the thunk instead of __tls_get_addr().
void *__tls_get_addr(TLVDescriptor *tlvd)
{
    // Make sure the thunk was initialized.  We know it should be
    // __tls_get_addr, so if it isn't, then tlv_initialize_descriptors didn't
    // get called.
#if 0 // skip check, save some instructions
    if (tlvd->thunk == 0 || tlvd->thunk == (void*)&_tlv_bootstrap) {
        return 0;
    }
#endif

    void* p = pthread_getspecific(tlvd->key);
    if (!p) {
        p = tlv_allocate_and_initialize_for_key(tlvd->key);
    }

    return p + tlvd->offset;
}
