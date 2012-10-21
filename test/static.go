package main

import (
	"net/http"
)

func main() {
	http.ListenAndServe(
		":8080",
		http.FileServer(http.Dir("/home/graham/Projects/netshare/test/")))
}
