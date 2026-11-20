package main

import "fmt"

//go:noinline
func depth(n int, pad [8]int) int {
	if n == 0 {
		return pad[0]
	}
	pad[n%8] += n
	return depth(n-1, pad) + pad[(n+1)%8]
}

func fib(n int) int {
	if n < 2 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

func main() {
	fmt.Println("depth 10:", depth(10, [8]int{}))
	fmt.Println("depth 5000:", depth(5000, [8]int{}))
	fmt.Println("depth 200000:", depth(200000, [8]int{}))

	done := make(chan int)
	for g := 0; g < 64; g++ {
		go func(g int) {
			done <- depth(1000+g*500, [8]int{g})
		}(g)
	}
	sum := 0
	for g := 0; g < 64; g++ {
		sum += <-done
	}
	fmt.Println("goroutines:", sum)
	fmt.Println("fib 27:", fib(27))
}
