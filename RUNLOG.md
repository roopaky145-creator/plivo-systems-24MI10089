### Profile A Run (200ms Grading Delay, seed 1)
```
endpoints done
relay done: {'up_bytes': 373800, 'down_bytes': 0, 'up_pkts': 2250, 'down_pkts': 0, 'dropped': 50, 'duplicated': 14}
================ SCORE ================
  frames               : 1500
  deadline misses      : 0  (0.00%)   [cap 1.00%]
  playout delay        : 200 ms   <-- your score if valid; lower wins
  bandwidth overhead   : 1.56x   [cap 2.00x]   (up 373800B, feedback 0B)
  RESULT               : VALID
```

### Profile A Run (200ms Grading Delay, seed 2 - Worst Case)
```
endpoints done
relay done: {'up_bytes': 372000, 'down_bytes': 0, 'up_pkts': 2250, 'down_pkts': 0, 'dropped': 43, 'duplicated': 15}
================ SCORE ================
  frames               : 1500
  deadline misses      : 0  (0.00%)   [cap 1.00%]
  playout delay        : 200 ms   <-- your score if valid; lower wins
  bandwidth overhead   : 1.55x   [cap 2.00x]   (up 372000B, feedback 0B)
  RESULT               : VALID
```

### Profile B Run (200ms Grading Delay, seed 1)
```
endpoints done
relay done: {'up_bytes': 375960, 'down_bytes': 180, 'up_pkts': 2274, 'down_pkts': 36, 'dropped': 125, 'duplicated': 25}
================ SCORE ================
  frames               : 1500
  deadline misses      : 5  (0.33%)   [cap 1.00%]
  playout delay        : 200 ms   <-- your score if valid; lower wins
  bandwidth overhead   : 1.57x   [cap 2.00x]   (up 375960B, feedback 180B)
  RESULT               : VALID
```

### Profile B Run (200ms Grading Delay, seed 2 - Worst Case)
```
endpoints done
relay done: {'up_bytes': 375795, 'down_bytes': 125, 'up_pkts': 2273, 'down_pkts': 25, 'dropped': 123, 'duplicated': 29}
================ SCORE ================
  frames               : 1500
  deadline misses      : 2  (0.13%)   [cap 1.00%]
  playout delay        : 200 ms   <-- your score if valid; lower wins
  bandwidth overhead   : 1.57x   [cap 2.00x]   (up 375795B, feedback 125B)
  RESULT               : VALID
```
