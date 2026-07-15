### Profile A Run (80ms, seed 1)
```
endpoints done
relay done: {'up_bytes': 376125, 'down_bytes': 125, 'up_pkts': 2275, 'down_pkts': 25, 'dropped': 53, 'duplicated': 14}
================ SCORE ================
  frames               : 1500
  deadline misses      : 1  (0.07%)   [cap 1.00%]
  playout delay        : 80 ms   <-- your score if valid; lower wins
  bandwidth overhead   : 1.57x   [cap 2.00x]   (up 376125B, feedback 125B)
  RESULT               : VALID
```

### Profile B Run (200ms, seed 1)
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
