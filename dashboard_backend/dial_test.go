package main

import (
	"net"
	"time"
)

type tcpConn struct{ net.Conn }

func dialTCP(host string) (*tcpConn, error) {
	c, err := net.DialTimeout("tcp", host, 2*time.Second)
	if err != nil {
		return nil, err
	}
	return &tcpConn{Conn: c}, nil
}
