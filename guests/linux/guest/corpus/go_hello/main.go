package main

import (
	"fmt"
	"os"
	"runtime"
)

func main() {
	fmt.Println("hello from go")
	fmt.Println("os:", runtime.GOOS, "arch:", runtime.GOARCH)
	fmt.Println("args:", len(os.Args))
	os.Exit(3)
}
