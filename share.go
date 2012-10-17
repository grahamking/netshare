package main

import (
	"io/ioutil"
	"log"
	"net"
	"os"
)

const (
	DEFAULT_ADDRESS   = "127.0.0.1:8080"
	DEFAULT_MIME_TYPE = "text/plain"
)

var (
	src  *os.File
	size int64
)

func main() {
	log.Println("Start")

	var oerr error

	src, oerr = os.Open("test/7000.txt")
	if oerr != nil {
		log.Fatal("Error opening payload. ", oerr)
	}

	fileinfo, serr := src.Stat()
	if serr != nil {
		log.Fatal("Error Stat on payload")
	}
	size = fileinfo.Size()
	log.Println("Payload size:", size)

	// Warm up OS page cache
	ioutil.ReadAll(src)
	src.Seek(0, os.SEEK_SET)

	sock, lerr := net.Listen("tcp", DEFAULT_ADDRESS)
	if lerr != nil {
		log.Fatal("Error listening on ", DEFAULT_ADDRESS, ". ", lerr)
	}

	for {
		conn, aerr := sock.Accept()

		if aerr != nil {
			log.Fatal("Error Accept. ", aerr)
		}

		go handle(conn)
	}
}

func handle(conn net.Conn) {

	var pos int64
	buf := make([]byte, 32*1024)

	for pos < size {

		read, rerr := src.ReadAt(buf, pos)
		log.Println("Read bytes:", read)
		log.Println("Err:", rerr)

		pos += int64(read)

		wrote, werr := conn.Write(buf[:read])
		log.Println("Wrote bytes:", wrote)
		log.Println("Err:", werr)
	}

	/*
		written, err := io.Copy(conn, src)
		src.Seek(0, os.SEEK_SET) // Rewind for next connection

		if err != nil {
			log.Fatal("Error copying to socket. ", err)
		}
		log.Println("Copied", written, "bytes to socket")
	*/
	conn.Close()
}
