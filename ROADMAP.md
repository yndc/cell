# Cell Memory Library Roadmap

## Planned

### Debug Features

- [ ] Bounds checking (guard bytes)
- [ ] Stack trace capture on allocation
- [ ] Leak detection report

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
| **0.7.0** | **TLS bin cache + Memory decommit ✓** ← Current |
| 1.0.0   | Production-ready with all debug features |
