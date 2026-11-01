# Paging

Subject: Live check (delete me)

## 1 Nov 2026, 16:21–16:51

Sources:
- <https://pages.cs.wisc.edu/~remzi/OSTEP/vm-paging.pdf>

### Paging
- page table maps virtual pages to frames
  - TLB caches recent translations

→ a TLB miss costs a page-table walk
- 4 KiB pages: offset = low 12 bits

