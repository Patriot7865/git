#ifndef PACK_OBJECTS_H
#define PACK_OBJECTS_H

#include "object-store.h"

#define OE_DFS_STATE_BITS	2
#define OE_DEPTH_BITS		12
#define OE_IN_PACK_BITS		10
#define OE_Z_DELTA_BITS		20

/*
 * State flags for depth-first search used for analyzing delta cycles.
 *
 * The depth is measured in delta-links to the base (so if A is a delta
 * against B, then A has a depth of 1, and B a depth of 0).
 */
enum dfs_state {
	DFS_NONE = 0,
	DFS_ACTIVE,
	DFS_DONE,
	DFS_NUM_STATES
};

/*
 * basic object info
 * -----------------
 * idx.oid is filled up before delta searching starts. idx.crc32 is
 * only valid after the object is written out and will be used for
 * generating the index. idx.offset will be both gradually set and
 * used in writing phase (base objects get offset first, then deltas
 * refer to them)
 *
 * "size" is the uncompressed object size. Compressed size of the raw
 * data for an object in a pack is not stored anywhere but is computed
 * and made available when reverse .idx is made.
 *
 * "hash" contains a path name hash which is used for sorting the
 * delta list and also during delta searching. Once prepare_pack()
 * returns it's no longer needed.
 *
 * source pack info
 * ----------------
 * The (in_pack, in_pack_offset) tuple contains the location of the
 * object in the source pack. in_pack_header_size allows quickly
 * skipping the header and going straight to the zlib stream.
 *
 * "type" and "in_pack_type" both describe object type. in_pack_type
 * may contain a delta type, while type is always the canonical type.
 *
 * deltas
 * ------
 * Delta links (delta, delta_child and delta_sibling) are created to
 * reflect that delta graph from the source pack then updated or added
 * during delta searching phase when we find better deltas.
 *
 * delta_child and delta_sibling are last needed in
 * compute_write_order(). "delta" and "delta_size" must remain valid
 * at object writing phase in case the delta is not cached.
 *
 * If a delta is cached in memory and is compressed, delta_data points
 * to the data and z_delta_size contains the compressed size. If it's
 * uncompressed [1], z_delta_size must be zero. delta_size is always
 * the uncompressed size and must be valid even if the delta is not
 * cached.
 *
 * [1] during try_delta phase we don't bother with compressing because
 * the delta could be quickly replaced with a better one.
 */
struct object_entry {
	struct pack_idx_entry idx;
	unsigned long size;	/* uncompressed size */
	unsigned in_pack_idx:OE_IN_PACK_BITS;	/* already in pack */
	off_t in_pack_offset;
	uint32_t delta_idx;	/* delta base object */
	uint32_t delta_child_idx; /* deltified objects who bases me */
	uint32_t delta_sibling_idx; /* other deltified objects who
				     * uses the same base as me
				     */
	void *delta_data;	/* cached delta (uncompressed) */
	unsigned long delta_size;	/* delta data size (uncompressed) */
	unsigned z_delta_size:OE_Z_DELTA_BITS;
	unsigned type_:TYPE_BITS;
	unsigned in_pack_type:TYPE_BITS; /* could be delta */
	unsigned type_valid:1;
	uint32_t hash;			/* name hint hash */
	unsigned char in_pack_header_size;
	unsigned preferred_base:1; /*
				    * we do not pack this, but is available
				    * to be used as the base object to delta
				    * objects against.
				    */
	unsigned no_try_delta:1;
	unsigned tagged:1; /* near the very tip of refs */
	unsigned filled:1; /* assigned write-order */
	unsigned dfs_state:OE_DFS_STATE_BITS;
	unsigned depth:OE_DEPTH_BITS;
};

struct packing_data {
	struct object_entry *objects;
	uint32_t nr_objects, nr_alloc;

	int32_t *index;
	uint32_t index_size;

	unsigned int *in_pack_pos;

	/*
	 * Only one of these can be non-NULL and they have different
	 * sizes. if in_pack_by_idx is allocated, oe_in_pack() returns
	 * the pack of an object using in_pack_idx field. If not,
	 * in_pack[] array is used the same way as in_pack_pos[]
	 */
	struct packed_git **in_pack_by_idx;
	struct packed_git **in_pack;
};

void prepare_packing_data(struct packing_data *pdata);
struct object_entry *packlist_alloc(struct packing_data *pdata,
				    const unsigned char *sha1,
				    uint32_t index_pos);

struct object_entry *packlist_find(struct packing_data *pdata,
				   const unsigned char *sha1,
				   uint32_t *index_pos);

static inline uint32_t pack_name_hash(const char *name)
{
	uint32_t c, hash = 0;

	if (!name)
		return 0;

	/*
	 * This effectively just creates a sortable number from the
	 * last sixteen non-whitespace characters. Last characters
	 * count "most", so things that end in ".c" sort together.
	 */
	while ((c = *name++) != 0) {
		if (isspace(c))
			continue;
		hash = (hash >> 2) + (c << 24);
	}
	return hash;
}

static inline enum object_type oe_type(const struct object_entry *e)
{
	return e->type_valid ? e->type_ : OBJ_BAD;
}

static inline void oe_set_type(struct object_entry *e,
			       enum object_type type)
{
	if (type >= OBJ_ANY)
		BUG("OBJ_ANY cannot be set in pack-objects code");

	e->type_valid = type >= OBJ_NONE;
	e->type_ = (unsigned)type;
}

static inline unsigned int oe_in_pack_pos(const struct packing_data *pack,
					  const struct object_entry *e)
{
	return pack->in_pack_pos[e - pack->objects];
}

static inline void oe_set_in_pack_pos(const struct packing_data *pack,
				      const struct object_entry *e,
				      unsigned int pos)
{
	pack->in_pack_pos[e - pack->objects] = pos;
}

static inline struct packed_git *oe_in_pack(const struct packing_data *pack,
					    const struct object_entry *e)
{
	if (pack->in_pack_by_idx)
		return pack->in_pack_by_idx[e->in_pack_idx];
	else
		return pack->in_pack[e - pack->objects];
}

void oe_map_new_pack(struct packing_data *pack,
		     struct packed_git *p);
static inline void oe_set_in_pack(struct packing_data *pack,
				  struct object_entry *e,
				  struct packed_git *p)
{
	if (!p->index)
		oe_map_new_pack(pack, p);
	if (pack->in_pack_by_idx)
		e->in_pack_idx = p->index;
	else
		pack->in_pack[e - pack->objects] = p;
}

static inline struct object_entry *oe_delta(
		const struct packing_data *pack,
		const struct object_entry *e)
{
	if (e->delta_idx)
		return &pack->objects[e->delta_idx - 1];
	return NULL;
}

static inline void oe_set_delta(struct packing_data *pack,
				struct object_entry *e,
				struct object_entry *delta)
{
	if (delta)
		e->delta_idx = (delta - pack->objects) + 1;
	else
		e->delta_idx = 0;
}

static inline struct object_entry *oe_delta_child(
		const struct packing_data *pack,
		const struct object_entry *e)
{
	if (e->delta_child_idx)
		return &pack->objects[e->delta_child_idx - 1];
	return NULL;
}

static inline void oe_set_delta_child(struct packing_data *pack,
				      struct object_entry *e,
				      struct object_entry *delta)
{
	if (delta)
		e->delta_child_idx = (delta - pack->objects) + 1;
	else
		e->delta_child_idx = 0;
}

static inline struct object_entry *oe_delta_sibling(
		const struct packing_data *pack,
		const struct object_entry *e)
{
	if (e->delta_sibling_idx)
		return &pack->objects[e->delta_sibling_idx - 1];
	return NULL;
}

static inline void oe_set_delta_sibling(struct packing_data *pack,
					struct object_entry *e,
					struct object_entry *delta)
{
	if (delta)
		e->delta_sibling_idx = (delta - pack->objects) + 1;
	else
		e->delta_sibling_idx = 0;
}

#endif
