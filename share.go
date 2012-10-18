package main

import (
	"bytes"
	"io"
	"io/ioutil"
	"log"
	"net"
	"os"
	"text/template"
)

const (
	DEFAULT_ADDRESS   = "127.0.0.1:8080"
	DEFAULT_MIME_TYPE = "image/jpeg"
	HEAD_TMPL         = "HTTP/1.0 200 OK\nCache-Control: max-age=31536000\nExpires: Thu, 31 Dec 2037 23:55:55 GMT\nContent-Type: {{.Mime}}\nContent-Length: {{.Length}}\n\n"
)

var (
	src     *os.File
	size    int64
	headers string
)

func main() {
	log.Println("Start")

	var oerr error

	src, oerr = os.Open("test/head.jpg")
	if oerr != nil {
		log.Fatal("Error opening payload. ", oerr)
	}

	fileinfo, serr := src.Stat()
	if serr != nil {
		log.Fatal("Error Stat on payload")
	}
	size = fileinfo.Size()
	log.Println("Payload size:", size)

	tmpl, terr := template.New("headers").Parse(HEAD_TMPL)
	if terr != nil {
		log.Fatal("Error parsing HEAD_TMPL", terr)
	}

	tmplData := struct {
		Mime   string
		Length int64
	}{DEFAULT_MIME_TYPE, size}

	headBuf := &bytes.Buffer{}
	terr = tmpl.Execute(headBuf, tmplData)
	if terr != nil {
		log.Fatal("Error executing header template", terr)
	}

	headers = headBuf.String()

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
	var read, wrote int
	var rerr, werr error
	buf := make([]byte, 32*1024)

	wrote, werr = conn.Write([]byte(headers))
	if werr != nil {
		log.Fatal("Error writing headers", werr)
	}

	for pos < size {

		read, rerr = src.ReadAt(buf, pos)
		if rerr != nil && rerr != io.EOF {
			log.Println("Read error:", rerr)
			break
		}
		//log.Println("Read bytes:", read)

		pos += int64(read)

		wrote, werr = conn.Write(buf[:read])
		if werr != nil {
			log.Println("Write error:", werr)
			break
		}
		//log.Println("Wrote bytes:", wrote)

		if wrote != read {
			log.Println("Read / write mismatch. Read ", read, ", wrote ", wrote)
		}
	}

	//log.Println("CloseWrite")
	werr = conn.(*net.TCPConn).CloseWrite()
	if werr != nil {
		log.Println("Error on Close", werr)
	}
}
