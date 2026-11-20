package main

import (
	"fmt"
	"sync"
)

func producer(n int, out chan<- int, wg *sync.WaitGroup) {
	defer wg.Done()
	for i := 0; i < n; i++ {
		out <- i
	}
}

func stage(in <-chan int, out chan<- int, f func(int) int) {
	for v := range in {
		out <- f(v)
	}
	close(out)
}

func main() {
	src := make(chan int)
	var wg sync.WaitGroup
	for p := 0; p < 8; p++ {
		wg.Add(1)
		go producer(500, src, &wg)
	}
	go func() {
		wg.Wait()
		close(src)
	}()

	sq := make(chan int, 16)
	inc := make(chan int, 16)
	go stage(src, sq, func(v int) int { return v * v })
	go stage(sq, inc, func(v int) int { return v + 1 })

	sum, count := 0, 0
	for v := range inc {
		sum += v
		count++
	}
	fmt.Println("values:", count, "sum:", sum)

	var mu sync.Mutex
	total := 0
	var wg2 sync.WaitGroup
	for g := 0; g < 1000; g++ {
		wg2.Add(1)
		go func(g int) {
			defer wg2.Done()
			for i := 0; i < 100; i++ {
				mu.Lock()
				total += g ^ i
				mu.Unlock()
			}
		}(g)
	}
	wg2.Wait()
	fmt.Println("mutex total:", total)

	ping, pong := make(chan int), make(chan int)
	go func() {
		for v := range ping {
			pong <- v + 1
		}
		close(pong)
	}()
	v := 0
	for i := 0; i < 20000; i++ {
		ping <- v
		v = <-pong
	}
	close(ping)
	fmt.Println("ping-pong:", v)
}
