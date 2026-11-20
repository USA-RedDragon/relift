package main

import (
	"errors"
	"fmt"
	"os"
)

type myErr struct{ code int }

func (e myErr) Error() string { return fmt.Sprint("myErr ", e.code) }

func safe(f func()) (err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("recovered: %v", r)
		}
	}()
	f()
	return nil
}

func nested(n int) {
	defer func() {
		if n == 3 {
			panic(fmt.Sprint("rethrown at ", n))
		}
	}()
	if n == 0 {
		var m map[string]int
		m["x"] = 1
	}
	nested(n - 1)
}

func main() {
	fmt.Println(safe(func() { panic("boom") }))
	fmt.Println(safe(func() {
		a := []int{1, 2, 3}
		i := 5
		_ = a[i]
	}))
	fmt.Println(safe(func() {
		x := 0
		fmt.Println(10 / x)
	}))
	fmt.Println(safe(func() { panic(myErr{7}) }))
	fmt.Println(safe(func() { nested(5) }))
	var e error = myErr{9}
	var m myErr
	fmt.Println(errors.As(e, &m), m.code)
	done := make(chan error)
	go func() { done <- safe(func() { panic("in goroutine") }) }()
	fmt.Println(<-done)
	defer fmt.Println("deferred before the crash")
	var arr []int
	if len(os.Args) > 5 {
		arr = append(arr, 1)
	}
	panic(fmt.Sprint("unrecovered ", len(arr)))
}
