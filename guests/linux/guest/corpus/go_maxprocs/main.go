package main

import (
	"fmt"
	"runtime"
	"sync"
)

func work(seed uint64, rounds int) uint64 {
	h := seed
	for i := 0; i < rounds; i++ {
		h ^= h << 13
		h ^= h >> 7
		h ^= h << 17
		h += uint64(i)
	}
	return h
}

func main() {
	fmt.Println("gomaxprocs:", runtime.GOMAXPROCS(0))
	var wg sync.WaitGroup
	out := make([]uint64, 16)
	for g := 0; g < len(out); g++ {
		wg.Add(1)
		go func(g int) {
			defer wg.Done()
			out[g] = work(uint64(g)+1, 2000000)
			runtime.Gosched()
			out[g] ^= work(out[g], 100000)
		}(g)
	}
	wg.Wait()
	var x uint64
	for _, v := range out {
		x ^= v
	}
	fmt.Printf("combined: %016x\n", x)

	results := make(chan int, 64)
	for w := 0; w < 8; w++ {
		go func(w int) {
			s := 0
			for i := 0; i < 300000; i++ {
				s += (i * w) % 7
			}
			results <- s
		}(w)
	}
	total := 0
	for w := 0; w < 8; w++ {
		total += <-results
	}
	fmt.Println("total:", total)
}
