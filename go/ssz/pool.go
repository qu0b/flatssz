package ssz

import "sync"

// HasherPool provides sync.Pool-based reuse of Hasher instances.
type HasherPool struct {
	pool sync.Pool
}

// DefaultHasherPool is the global hasher pool used by generated code.
var DefaultHasherPool HasherPool

// Get acquires a Hasher from the pool, creating one if needed.
func (p *HasherPool) Get() *Hasher {
	h := p.pool.Get()
	if h == nil {
		return NewHasher()
	}
	return h.(*Hasher)
}

// Put returns a Hasher to the pool after resetting it.
func (p *HasherPool) Put(h *Hasher) {
	h.Reset()
	p.pool.Put(h)
}
