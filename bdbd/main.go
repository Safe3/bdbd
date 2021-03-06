package main

import (
	"github.com/BurntSushi/toml"
	"github.com/nybuxtsui/bdbd/bdb"
	"github.com/nybuxtsui/bdbd/server"
	mylog "github.com/nybuxtsui/log"
	"io/ioutil"
	"log"
	"net"
	"os"
	"os/signal"
	"runtime"
	"syscall"
)

//var redisAddr = flag.String("l", ":2323", "redis listen port")

import "C"

func main() {
	runtime.GOMAXPROCS(runtime.NumCPU() * 4)

	signalChan := make(chan os.Signal, 1)
	signal.Notify(signalChan, syscall.SIGINT, syscall.SIGTERM)

	// master
	//go run main.go -h db1 -M -l 127.0.0.1:2345 -r 127.0.0.1:2346
	// slave
	//go run main.go -h db2 -C -l 127.0.0.1:2346 -r 127.0.0.1:2345

	configFile := "bdbd.conf"
	if len(os.Args) > 1 {
		configFile = os.Args[1]
	}
	configstr, err := ioutil.ReadFile(configFile)
	if err != nil {
		log.Println("ERROR: read config file failed:", err)
		os.Exit(1)
	}
	var config struct {
		Bdb    bdb.BdbConfig        `toml:"bdb"`
		Logger []mylog.LoggerDefine `toml:"logger"`
		Server struct {
			Listen string
		} `toml:"server"`
	}
	config.Bdb.Flush = 1
	_, err = toml.Decode(string(configstr), &config)
	if err != nil {
		log.Println("ERROR: decode config failed:", err)
		os.Exit(1)
	}

	mylog.Init(config.Logger)

	mylog.Info("bdb|starting")
	dbenv := bdb.Start(config.Bdb)
	mylog.Info("bdb|started")

	/*
		db, err := dbenv.GetDb("test")
		if err != nil {
			mylog.Fatal("dberr:%s", err.Error())
		}
		for i := 0; i < 1000000; i++ {
			if i%1000 == 0 {
				mylog.Info("%d", i)
			}
			k := fmt.Sprintf("k_%v", i)
			db.Set([]byte(k), []byte(k))
		}
		db.Close()
		dbenv.Exit()
		mylog.Fatal("end")
	*/

	go func() {
		addr, err := net.ResolveTCPAddr("tcp", config.Server.Listen)
		if err != nil {
			mylog.Fatal("ResolveTCPAddr|%s", err.Error())
		}
		listener, err := net.ListenTCP("tcp", addr)
		if err != nil {
			mylog.Fatal("Server|ListenTCP|%s", err.Error())
		}
		for {
			client, err := listener.AcceptTCP()
			if err != nil {
				mylog.Error("Server|%s", err.Error())
			}
			client.SetKeepAlive(true)
			conn := server.NewConn(client, dbenv)
			go conn.Start()
		}
	}()

	server.Start(dbenv)
	mylog.Info("start")
	<-signalChan
	server.Exit()
	dbenv.Exit()
	mylog.Info("bye")
}
