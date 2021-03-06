package server

import (
	"bufio"
	"errors"
	"fmt"
	"github.com/nybuxtsui/bdbd/bdb"
	"github.com/nybuxtsui/log"
	"io"
	"net"
	"runtime"
	"strings"
)

type Conn struct {
	conn net.Conn

	rb *bufio.Reader
	wb *bufio.Writer

	dbenv *bdb.DbEnv
	dbmap map[string]*bdb.Db
}

var (
	ErrRequest = errors.New("invalid request")
)

func NewConn(c net.Conn, dbenv *bdb.DbEnv) *Conn {
	return &Conn{
		conn:  c,
		rb:    bufio.NewReaderSize(c, 1024*1024),
		wb:    bufio.NewWriterSize(c, 16*1024),
		dbenv: dbenv,
		dbmap: make(map[string]*bdb.Db),
	}
}

func (c *Conn) readLine() ([]byte, error) {
	line, err := c.rb.ReadSlice('\n')
	if err != nil {
		log.Error("readLine|ReadSlice|%s", err.Error())
		return nil, err
	}
	if len(line) <= 2 {
		log.Error("readLine|empty")
		return nil, ErrRequest
	}
	if line[len(line)-2] != '\r' {
		log.Error("readLine|invalid")
		return nil, ErrRequest
	}
	return line[:len(line)-2], nil
}

func readNumber(buff []byte) (int64, error) {
	if len(buff) == 0 {
		log.Error("readNumber|empty")
		return 0, ErrRequest
	}
	var sign int64 = 1
	var r int64 = 0
	for i, c := range buff {
		if i == 0 && c == '-' {
			sign = -1
		} else if c < '0' || c > '9' {
			log.Error("readNumber|invalid|%v", int(c))
			return 0, ErrRequest
		} else {
			r *= 10
			r += int64(c - '0')
		}
	}
	return r * sign, nil
}

func (c *Conn) readCount(tag byte) (int64, error) {
	line, err := c.readLine()
	if err != nil {
		log.Error("readCount|readLine|%s", err.Error())
		return 0, err
	}
	if len(line) < 2 {
		log.Error("readCount|invalid|%v", line)
		return 0, ErrRequest
	}
	if line[0] != tag {
		log.Error("readCount|tag|%c", line[0])
		return 0, ErrRequest
	}
	count, err := readNumber(line[1:])
	if err != nil {
		log.Error("readCount|readNumber|%s", err.Error())
		return 0, ErrRequest
	}
	return count, nil
}

func (c *Conn) processRequest() error {
	req, err := c.readRequest()
	if err != nil {
		log.Error("processRequest|readRequest|%s", err.Error())
		return err
	}
	cmd := strings.ToLower(string(req[0]))
	if def, ok := cmdMap[cmd]; ok {
		args := req[1:]
		if len(args) < def.minArgs || len(args) > def.maxArgs {
			c.wb.WriteString(fmt.Sprintf("-ERR wrong number of arguments for '%s' command\r\n", cmd))
		} else {
			err = def.fun(c, args)
			if err != nil {
				log.Error("processRequest|func|%s", err.Error())
				return err
			}
		}
	} else {
		c.wb.WriteString(fmt.Sprintf("-ERR unknown command '%s'\r\n", cmd))
	}
	if err = c.wb.Flush(); err != nil {
		log.Error("processRequest|Flush|%s", err.Error())
		return err
	} else {
		return nil
	}
}

func (c *Conn) readRequest() ([][]byte, error) {
	count, err := c.readCount('*')
	if err != nil {
		log.Error("readRequest|readCount|%s", err.Error())
		return nil, err
	}
	if count <= 0 {
		log.Error("readRequest|invalid_count|%v", count)
		return nil, ErrRequest
	}
	var r = make([][]byte, count)
	var i int64
	for i = 0; i < count; i++ {
		length, err := c.readCount('$')
		if err != nil {
			log.Error("readRequest|readCount|%s", err.Error())
			return nil, err
		}
		buff := make([]byte, length+2)
		_, err = io.ReadFull(c.rb, buff)
		if err != nil {
			log.Error("readRequest|ReadFull|%s", err.Error())
			return nil, err
		}
		if buff[length+1] != '\n' || buff[length] != '\r' {
			log.Error("readRequest|invalid_crlf|%v", buff)
			return nil, ErrRequest
		}
		r[i] = buff[0:length]
	}
	return r, nil
}

func (conn *Conn) writeLen(prefix byte, n int) error {
	var buff [64]byte
	buff[len(buff)-1] = '\n'
	buff[len(buff)-2] = '\r'
	i := len(buff) - 3
	for {
		buff[i] = byte('0' + n%10)
		i -= 1
		n = n / 10
		if n == 0 {
			break
		}
	}
	buff[i] = prefix
	_, err := conn.wb.Write(buff[i:])
	return err
}

func (c *Conn) Close() {
	c.conn.Close()
}

func PrintPanic(err interface{}) {
	if err != nil {
		log.Error("* panic:|%v", err)
	}
	for skip := 2; ; skip++ {
		_, file, line, ok := runtime.Caller(skip)
		if !ok {
			break
		}
		fmt.Printf("*  %v : %v\n", file, line)
	}
}

func (c *Conn) Start() {
	defer func() {
		c.Close()
		for _, v := range c.dbmap {
			v.Close()
		}
		if err := recover(); err != nil {
			PrintPanic(err)
		}
	}()
	for {
		err := c.processRequest()
		if err != nil {
			log.Error("Start|processRequest|%s", err.Error())
			break
		}
	}
}
