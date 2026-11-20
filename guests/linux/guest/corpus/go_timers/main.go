package main

import (
	"fmt"
	"time"
)

func main() {
	start := time.Now()
	tick := time.NewTicker(10 * time.Millisecond)
	defer tick.Stop()
	timeout := time.After(105 * time.Millisecond)
	n := 0
loop:
	for {
		select {
		case <-tick.C:
			n++
		case <-timeout:
			break loop
		}
	}
	fmt.Println("ticks at least 8:", n >= 8)

	res := make(chan string)
	go func() {
		time.Sleep(30 * time.Millisecond)
		res <- "slow"
	}()
	select {
	case r := <-res:
		fmt.Println("got", r)
	case <-time.After(5 * time.Millisecond):
		fmt.Println("timed out first")
	}
	fmt.Println("then", <-res)

	t := time.NewTimer(time.Hour)
	fmt.Println("stopped:", t.Stop())
	t.Reset(5 * time.Millisecond)
	<-t.C
	fmt.Println("reset fired")

	fired := make(chan int, 3)
	for i := 3; i >= 1; i-- {
		i := i
		time.AfterFunc(time.Duration(i)*8*time.Millisecond, func() { fired <- i })
	}
	fmt.Println("order:", <-fired, <-fired, <-fired)

	el := time.Since(start)
	fmt.Println("elapsed sane:", el >= 100*time.Millisecond && el < 5*time.Second)
}
