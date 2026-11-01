# Prefix sum

Subject: Live check (delete me)

Time studied: 45 min

## 30 Oct 2026, 17:21–17:51

Sources:
- <https://usaco.guide/silver/prefix-sums>
- <https://cp-algorithms.com/>

### Prefix sum
- pre[0] = 0, pre[i] = pre[i-1] + a[i]
  - range sum (l, r) = pre[r] - pre[l-1]
  - works with negative numbers & zeros

→ O(1) per query after O(n) setup
#### Edge cases
- empty array: "no queries" — l > r is invalid
- café, naïve, 1 ≤ k ≤ N, \<tags> and *stars* stay text

Plain text line with a URL https://cp-algorithms.com/data_structures/prefix_sum.html in it.

## 31 Oct 2026, 17:21–17:51

#### 2D prefix sums
- P[i][j] = a[i][j] + P[i-1][j] + P[i][j-1] - P[i-1][j-1]
  - inclusion–exclusion

→ submatrix sum in O(1)

