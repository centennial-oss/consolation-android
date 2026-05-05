/*
 * Fixed-capacity FIFO ring for pointer-sized elements (e.g. uvc_frame_t *).
 * Enqueue with bounded storage: on overflow, drops the oldest slot and returns it
 * so the caller can recycle/free it — O(1) per operation, no shifts.
 *
 * Not thread-safe; external mutex required (same contract as prior ObjectArray queue).
 */

#ifndef BOUNDED_POINTER_RING_H_
#define BOUNDED_POINTER_RING_H_

#include <cstring>

#include "utilbase.h"

template<typename T>
class BoundedPointerRing {
private:
	T *ring_slots;
	int ring_cap;
	int ring_head;
	int ring_count;

public:
	explicit BoundedPointerRing(int capacity)
		: ring_slots(nullptr),
		  ring_cap(0),
		  ring_head(0),
		  ring_count(0) {
		if (capacity > 0) {
			ring_slots = new T[static_cast<size_t>(capacity)]();
			ring_cap = capacity;
		}
	}

	BoundedPointerRing(const BoundedPointerRing &) = delete;
	BoundedPointerRing &operator=(const BoundedPointerRing &) = delete;

	~BoundedPointerRing() { delete[] ring_slots; }

	inline int size() const { return ring_count; }
	inline bool empty() const { return ring_count < 1; }
	inline bool full() const { return ring_cap > 0 && ring_count >= ring_cap; }

	/**
	 * Append one item. If the ring was full, returns the displaced oldest item
	 * (caller owns it); otherwise nullptr.
	 */
	T enqueue_drop_oldest_if_full(T incoming) {
		T dropped = nullptr;
		if UNLIKELY(ring_cap < 1 || !ring_slots)
			return incoming;
		if (ring_count >= ring_cap) {
			dropped = ring_slots[ring_head];
			ring_head = (ring_head + 1) % ring_cap;
			ring_count--;
		}
		const int tail = (ring_head + ring_count) % ring_cap;
		ring_slots[tail] = incoming;
		ring_count++;
		return dropped;
	}

	T dequeue() {
		if (empty())
			return nullptr;
		T out = ring_slots[ring_head];
		ring_head = (ring_head + 1) % ring_cap;
		ring_count--;
		return out;
	}

	/** Drop ownership of stored pointers without freeing them (caller must drain first). */
	void reset_storage() {
		ring_head = 0;
		ring_count = 0;
		if (ring_slots && ring_cap > 0)
			std::memset(ring_slots, 0,
				static_cast<size_t>(ring_cap) * sizeof(T));
	}
};

#endif /* BOUNDED_POINTER_RING_H_ */
