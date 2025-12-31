# Cell Memory Library Roadmap

## Planned

### Debug Features

- [x] Bounds checking (guard bytes)
- [x] Stack trace capture on allocation
- [x] Leak detection report

### API Enhancements

- [ ] Aligned allocation API (`alloc_aligned`)
- [ ] Realloc support (`realloc_bytes`)
- [ ] STL-compatible allocator wrapper

### Production Hardening

- [ ] Thread safety audit
- [ ] Memory budget limits
- [ ] Allocation callbacks for instrumentation

---

## Version Milestones

| Version | Features |
|---------|----------|
| 0.1.0   | Cell allocator (Layer 1) ✓ |
| 0.2.0   | Sub-cell allocator (Layer 2) ✓ |
| 0.3.0   | Arena allocator ✓ |
| 0.4.0   | Pool\<T\> + ArenaScope ✓ |
| 0.5.0   | Buddy + Large allocations ✓ |
| 0.6.0   | Memory statistics ✓ |
| 0.7.0   | TLS bin cache + Memory decommit ✓ |
| **0.8.0** | **Debug features (guards, stack traces, leaks) ✓** ← Current |
| 1.0.0   | Production-ready with all API enhancements |
