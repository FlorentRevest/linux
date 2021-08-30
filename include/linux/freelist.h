/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
#ifndef FREELIST_H
#define FREELIST_H

#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/atomic.h>

/*
 * freelist: a lock-less version of object pool implementation
 *
 * Copyright: cameron@moodycamel.com, wuqiang.matt@bytedance.com
 *
 * The object pool is a scalable implementaion of high performance queue
 * for objects allocation and reclamation, such as kretprobe instances.
 *
 * It's basd on cameron's CAS-based lock-free freelist:
 * https://moodycamel.com/blog/2014/solving-the-aba-problem-for-lock-free-free-lists
 *
 * With leveraging per-cpu lockless queue to mitigate hot spots of memory
 * contention, it could deliver near-linear scalability for high parallel
 * loads. The object pool are best suited for the following cases:
 * 1) memory allocation or reclamation is prohibited or too expensive
 * 2) the objects are allocated/used/reclaimed very frequently
 *
 * Before using, you must be aware of it's limitations:
 * 1) Memory of all objects won't be freed until pool is de-allocated
 * 2) Order and fairness are not guaranteed. So some threads might stay
 *    hungry much longer than other competitors
 *
 * Objects could be pre-allocated during initialization or filled later
 * with user's buffer or private allocations. Mixing different objects
 * of self-managed/batched/manually-added is NOT recommended, though
 * it's supported. For mixed case, the caller should take care of the
 * releasing of objects or user pool.
 *
 * Typical use cases:
 *
 * 1) self-managed objects
 *
 * obj_init():
 *	static int obj_init(void *context, struct freelist_node *obj)
 *	{
 *		struct my_node *node;
 *		node = container_of(obj, struct my_node, obj);
 *		do_init_node(context, node);
 *		return 0;
 *	}
 *
 * main():
 *	freelist_init(&fh, num_possible_cpus() * 4, 16, GFP_KERNEL, context, obj_init);
 *	<object pool initialized>
 *
 *	obj = freelist_pop(&fh);
 *	do_something_with(obj);
 *	freelist_push(obj, &fh);
 *
 *	<object pool to be destroyed>
 *	freelist_fini(&fh, NULL, NULL);
 *
 * 2) batced with user's buffer
 *
 * obj_init():
 *	static int obj_init(void *context, struct freelist_node *obj)
 *	{
 *		struct my_node *node;
 *		node = container_of(obj, struct my_node, obj);
 *		do_init_node(context, node);
 *		return 0;
 *	}
 *
 * free_buf():
 *	static int free_buf(void *context, void *obj, int user, int element)
 *	{
 *		if (obj && user && !element)
 *			kfree(obj);
 *	}
 *
 * main():
 *	freelist_init(&fh, num_possible_cpus() * 4, 0, GFP_KERNEL, 0, 0);
 *	buffer = kmalloc(size, ...);
 *	freelist_populate(&fh, buffer, size, 16, context, obj_init);
 *	<object pool initialized>
 *
 *	obj = freelist_pop(&fh);
 *	do_something_with(obj);
 *	freelist_push(obj, &fh);
 *
 *	<object pool to be destroyed>
 *	freelist_fini(&fh, context, free_buf);
 *
 * 3) manually added with user objects
 *
 * free_obj():
 *	static int free_obj(void *context, void *obj, int user, int element)
 *	{
 *		struct my_node *node;
 *		node = container_of(obj, struct my_node, obj);
 *		if (obj && user && element)
 *			kfree(node);
 *	}
 *
 * main():
 *	freelist_init(&fh, num_possible_cpus() * 4, 0, 0, GFP_KERNEL, 0, 0);
 *	for () {
 *		node = kmalloc(objsz, ...);
 *		do_init_node(node);
 *		freelist_add_scattered(&node.obj, oh);
 *	}
 *	<object pool initialized>
 *
 *	obj = freelist_pop(&fh);
 *	do_something_with(obj);
 *	freelist_push(obj, &fh);
 *
 *	<object pool to be destroyed>
 *	freelist_fini(&fh, context, free_obj);
 */

/*
 * common componment of every node
 */
struct freelist_node {
	struct freelist_node   *next;
	atomic_t                refs;
};

#define REFS_ON_FREELIST 0x80000000
#define REFS_MASK	 0x7FFFFFFF

/*
 * freelist_slot: per-cpu singly linked list
 *
 * All pre-allocated objects are next to freelist_slot. Objects and
 * freelist_slot are to be allocated from the memory pool local node.
 */
struct freelist_slot {
	struct freelist_node   *fs_head;	/* head of percpu list */
};
#define SLOT_OBJS(s) ((void *)(s) + sizeof(struct freelist_slot))

/*
 * freelist_head: object pooling metadata
 */
struct freelist_head {
	uint32_t                fh_objsz;	/* object & element size */
	uint32_t                fh_nobjs;	/* total objs in freelist */
	uint32_t                fh_ncpus;	/* num of possible cpus */
	uint32_t                fh_in_slot:1;	/* objs alloced with slots */
	uint32_t                fh_vmalloc:1;	/* alloc from vmalloc zone */
	gfp_t                   fh_gfp;		/* k/vmalloc gfp flags */
	uint32_t                fh_sz_pool;	/* user pool size in byes */
	void                   *fh_pool;	/* user managed memory pool */
	struct freelist_slot  **fh_slots;	/* array of percpu slots */
	uint32_t               *fh_sz_slots;	/* size in bytes of slots */
};

typedef int (*freelist_init_node_cb)(void *context, struct freelist_node *);

/* attach object to percpu slot */
static inline void
__freelist_insert_node(struct freelist_node *node, struct freelist_slot *slot)
{
	atomic_set_release(&node->refs, 1);
	node->next = slot->fs_head;
	slot->fs_head = node;
}

/* allocate and initialize percpu slots */
static inline int
__freelist_init_slots(struct freelist_head *head, uint32_t nobjs,
			void *context, freelist_init_node_cb objinit)
{
	uint32_t i, objsz, cpus = head->fh_ncpus;
	gfp_t gfp = head->fh_gfp;

	/* allocate array for percpu slots */
	head->fh_slots = kzalloc(cpus * sizeof(uint32_t) +
				cpus * sizeof(void *), gfp);
	if (!head->fh_slots)
		return -ENOMEM;
	head->fh_sz_slots = (uint32_t *)&head->fh_slots[cpus];

	/* aligned object size by sizeof(void *) */
	objsz = ALIGN(head->fh_objsz, sizeof(void *));

	/* shall we allocate objects along with freelist_slot */
	if (objsz)
		head->fh_in_slot = 1;

	/* initialize per-cpu slots */
	for (i = 0; i < cpus; i++) {
		struct freelist_slot *slot;
		uint32_t j, n, s;

		/* compute how many objects to be managed by this slot */
		n = nobjs / cpus;
		if (i < (nobjs % cpus))
			n++;
		s = sizeof(struct freelist_slot) + objsz * n;

		/* decide which zone shall the slot be allocated from */
		if (0 == i) {
			if ((gfp & GFP_ATOMIC) || s < PAGE_SIZE)
				head->fh_vmalloc = 0;
			else
				head->fh_vmalloc = 1;
		}

		/* allocate percpu slot & objects from local memory */
		if (head->fh_vmalloc)
			slot = __vmalloc_node(s, 1, gfp, cpu_to_node(i),
					__builtin_return_address(0));
		else
			slot = kmalloc_node(s, gfp, cpu_to_node(i));
		if (!slot)
			return -ENOMEM;

		head->fh_slots[i] = slot;
		head->fh_sz_slots[i] = s;

		/* initialize percpu slot for the i-th cpu */
		memset(slot, 0, s);
		/* initialize pre-allocated record entries */
		for (j = 0; head->fh_in_slot && j < n; j++) {
			struct freelist_node *node;
			node = SLOT_OBJS(slot) + j * objsz;
			if (objinit) {
				int rc = objinit(context, node);
				if (rc)
					return rc;
			}
			__freelist_insert_node(node, slot);
			head->fh_nobjs++;
		}
	}

	return 0;
}

/* cleanup all percpu slots of the object pool */
static inline void __freelist_fini_slots(struct freelist_head *head)
{
	uint32_t i;

	if (!head->fh_slots)
		return;

	for (i = 0; i < head->fh_ncpus; i++) {
		if (!head->fh_slots[i])
			continue;
		if (head->fh_vmalloc)
			vfree(head->fh_slots[i]);
		else
			kfree(head->fh_slots[i]);
	}
	kfree(head->fh_slots);
	head->fh_slots = NULL;
	head->fh_sz_slots = NULL;
}

/**
 * freelist_init: initialize object pool and pre-allocate objects
 *
 * args:
 * @fh:    the object pool to be initialized, declared by the caller
 * @nojbs: total objects to be managed by this object pool
 * @ojbsz: size of an object, to be pre-allocated if objsz is not 0
 * @gfp:   gfp flags of caller's context for memory allocation
 * @context: user context for object initialization callback
 * @objinit: object initialization callback
 *
 * return:
 *         0 for success, otherwise error code
 *
 * All pre-allocated objects are to be zeroed. Caller should do extra
 * initialization before using.
 */
static inline int
freelist_init(struct freelist_head *head, int nobjs, int objsz, gfp_t gfp,
		void *context, freelist_init_node_cb objinit)
{
	memset(head, 0, sizeof(struct freelist_head));
	head->fh_ncpus = num_possible_cpus();
	head->fh_objsz = objsz;
	head->fh_gfp = gfp & ~__GFP_ZERO;

	if (__freelist_init_slots(head, nobjs, context, objinit)) {
		__freelist_fini_slots(head);
		return -ENOMEM;
	}

	return 0;
}

/**
 * freelist_add_scattered: adding pre-allocated objects to objects pool
 * during initialization. it will try to balance the object numbers of
 * all slots.
 *
 * args:
 * @node: object pointer to be added to object pool
 * @head: object pool
 *
 * return:
 *     0 or error code
 *
 * freelist_add_scattered doesn't handle race conditions, can only be
 * called during object pool initialization
 */
static inline int
freelist_add_scattered(struct freelist_node *node, struct freelist_head *head)
{
	uint32_t cpu;

	/* try balance object numbers among slots */
	cpu = head->fh_nobjs % head->fh_ncpus;
	__freelist_insert_node(node, head->fh_slots[cpu]);
	head->fh_nobjs++;

	return 0;
}

/**
 * freelist_populate: add objects from user provided pool in batch
 *  *
 * args:
 * @oh:  object pool
 * @buf: user buffer for pre-allocated objects
 * @size: size of user buffer
 * @objsz: size of object & element
 * @context: user context for objinit callback
 * @objinit: object initialization callback
 *
 * return:
 *     0 or error code
 */
static inline int
freelist_populate(struct freelist_head *head, void *buf, int size, int objsz,
		void *context, freelist_init_node_cb objinit)
{
	int used = 0;

	if (head->fh_pool || !buf || !objsz || size < objsz)
		return -EINVAL;
	if (head->fh_objsz && head->fh_objsz != objsz)
		return -EINVAL;

	WARN_ON_ONCE(((unsigned long)buf) & (sizeof(void *) - 1));
	WARN_ON_ONCE(((uint32_t)objsz) & (sizeof(void *) - 1));

	while (used + objsz <= size) {
		struct freelist_node *node = buf + used;
		if (objinit) {
			int rc = objinit(context, node);
			if (rc)
				return rc;
		}
		if (freelist_add_scattered(node, head))
			break;
		used += objsz;
	}

	if (!used)
		return -ENOENT;

	head->fh_pool = buf;
	head->fh_sz_pool = size;
	head->fh_objsz = objsz;

	return 0;
}

static inline void __freelist_cas_add(struct freelist_node *node, struct freelist_slot *slot)
{
	/*
	 * Since the refcount is zero, and nobody can increase it once it's
	 * zero (except us, and we run only one copy of this method per node at
	 * a time, i.e. the single thread case), then we know we can safely
	 * change the next pointer of the node; however, once the refcount is
	 * back above zero, then other threads could increase it (happens under
	 * heavy contention, when the refcount goes to zero in between a load
	 * and a refcount increment of a node in try_get, then back up to
	 * something non-zero, then the refcount increment is done by the other
	 * thread) -- so if the CAS to add the node to the actual list fails,
	 * decrese the refcount and leave the add operation to the next thread
	 * who puts the refcount back to zero (which could be us, hence the
	 * loop).
	 */
	struct freelist_node *head = READ_ONCE(slot->fs_head);

	for (;;) {
		WRITE_ONCE(node->next, head);
		atomic_set_release(&node->refs, 1);

		if (try_cmpxchg_release(&slot->fs_head, &head, node))
			break;

		/*
		 * Hmm, the add failed, but we can only try again when refcount
		 * goes back to zero (with REFS_ON_FREELIST set).
		 */
		if (atomic_fetch_add_release(REFS_ON_FREELIST - 1, &node->refs) != 1)
			break;
	}
}

/* adding object to slot */
static inline int __freelist_add_slot(struct freelist_node *node, struct freelist_slot *slot)
{
	/*
	 * We know that the should-be-on-freelist bit is 0 at this point, so
	 * it's safe to set it using a fetch_add.
	 */
	if (!atomic_fetch_add_release(REFS_ON_FREELIST, &node->refs)) {
		/*
		 * Oh look! We were the last ones referencing this node, and we
		 * know we want to add it to the free list, so let's do it!
		 */
		__freelist_cas_add(node, slot);
	}

	return 0;
}

/**
 * freelist_push: reclaim the object and return back to objects pool
 *
 * args:
 * @node: object pointer to be pushed to object pool
 * @head: object pool
 *
 * return:
 *     0 (freelist_push never fail)
 *
 * freelist_push() can be nested (irp/softirq/preemption)
 */
static inline int freelist_push(struct freelist_node *node, struct freelist_head *head)
{
	int cpu = raw_smp_processor_id();
	return __freelist_add_slot(node, head->fh_slots[cpu]);
}

/* try to retrieve object from slot */
static inline struct freelist_node *__freelist_pop_slot(struct freelist_slot *slot)
{
	struct freelist_node *prev, *next, *head = smp_load_acquire(&slot->fs_head);
	unsigned int refs;

	while (head) {
		prev = head;
		refs = atomic_read(&head->refs);
		if ((refs & REFS_MASK) == 0 ||
		    !atomic_try_cmpxchg_acquire(&head->refs, &refs, refs+1)) {
			head = smp_load_acquire(&slot->fs_head);
			continue;
		}

		/*
		 * Good, reference count has been incremented (it wasn't at
		 * zero), which means we can read the next and not worry about
		 * it changing between now and the time we do the CAS.
		 */
		next = READ_ONCE(head->next);
		if (try_cmpxchg_acquire(&slot->fs_head, &head, next)) {
			/*
			 * Yay, got the node. This means it was on the list,
			 * which means should-be-on-freelist must be false no
			 * matter the refcount (because nobody else knows it's
			 * been taken off yet, it can't have been put back on).
			 */
			WARN_ON_ONCE(atomic_read(&head->refs) & REFS_ON_FREELIST);

			/*
			 * Decrease refcount twice, once for our ref, and once
			 * for the list's ref.
			 */
			atomic_fetch_add(-2, &head->refs);

			return head;
		}

		/*
		 * OK, the head must have changed on us, but we still need to decrement
		 * the refcount we increased.
		 */
		refs = atomic_fetch_add(-1, &prev->refs);
		if (refs == REFS_ON_FREELIST + 1)
			__freelist_cas_add(prev, slot);
	}

	return NULL;
}

/**
 * freelist_pop: allocate an object from objects pool
 *
 * args:
 * @head: object pool
 *
 * return:
 *   node: NULL if failed (object pool is empty)
 *
 * freelist_pop can be nesed, and guaranteed to be deadlock-free.
 * So it can be called in any context, like irq/softirq/nmi.
 */
static inline struct freelist_node *freelist_pop(struct freelist_head *head)
{
	struct freelist_node *node;
	int i, cpu = raw_smp_processor_id();

	for (i = 0; i < head->fh_ncpus; i++) {
		struct freelist_slot *slot;
		slot = head->fh_slots[cpu];
		node = __freelist_pop_slot(slot);
		if (node)
			return node;
		if (++cpu >= head->fh_ncpus)
			cpu = 0;
	}

	return NULL;
}

/* whether this object is from user buffer (batched adding) */
static inline int freelist_is_inpool(void *obj, struct freelist_head *fh)
{
	return (obj && obj >= fh->fh_pool &&
		obj < fh->fh_pool + fh->fh_sz_pool);
}

/* whether this object is pre-allocated with percpu slots */
static inline int freelist_is_inslot(void *obj, struct freelist_head *fh)
{
	uint32_t i;

	for (i = 0; i < fh->fh_ncpus; i++) {
		void *ptr = fh->fh_slots[i];
		if (obj && obj >= ptr && obj < ptr + fh->fh_sz_slots[i])
			return 1;
	}

	return 0;
}

/**
 * freelist_fini: cleanup the whole object pool (releasing all objects)
 *
 * args:
 * @head: object pool
 * @context: user provided value for the callback of release() funciton
 * @release: user provided callback for resource cleanup or statistics
 *
 * prototype of release callback:
 * static int release(void *context, void *obj, int user, int element);
 * args:
 *  context: user provided value
 *  obj: the object (element or buffer) to be cleaned up
 *  user: the object is manually provided by user
 *  element: obj is an object or user-provided buffer
 */
static inline void freelist_fini(struct freelist_head *head, void *context,
				int (*release)(void *, void *, int, int))
{
	uint32_t i;

	if (!head->fh_slots)
		return;

	for (i = 0; release && i < head->fh_ncpus; i++) {
		void *obj;
		if (!head->fh_slots[i])
			continue;
		do {
			obj = __freelist_pop_slot(head->fh_slots[i]);
			if (obj) {
				int user = !freelist_is_inpool(obj, head) &&
					!freelist_is_inslot(obj, head);
				release(context, obj, user, 1);
			}
		} while (obj);
	}

	if (head->fh_pool && release) {
		release(context, head->fh_pool, 1, 0);
		head->fh_pool = NULL;
		head->fh_sz_pool = 0;
	}

	__freelist_fini_slots(head);
}

#endif /* FREELIST_H */
