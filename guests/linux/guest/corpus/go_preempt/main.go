package main

import (
	"fmt"
	"runtime"
	"sync/atomic"
	"time"
)

func spin(n *atomic.Int64) uint64 {
	var h uint64 = 1469598103934665603
	for {
		h ^= h >> 7
		h *= 1099511628211
		if h == 0 {
			return h
		}
		n.Add(1)
	}
}

func main() {
	runtime.GOMAXPROCS(1)
	var count atomic.Int64
	for g := 0; g < 3; g++ {
		go spin(&count)
	}
	for i := 0; i < 5; i++ {
		time.Sleep(10 * time.Millisecond)
	}
	fmt.Println("main kept running with spinners:", count.Load() > 0)

	var plain uint64
	go func() {
		for {
			plain++
		}
	}()
	time.Sleep(20 * time.Millisecond)
	fmt.Println("main woke while a plain loop spins")

	deadline := time.Now().Add(60 * time.Millisecond)
	n := 0
	for time.Now().Before(deadline) {
		n++
	}
	fmt.Println("loop done:", n > 0)
}
