bdbd
====
bdbd是采用redis作为通讯协议的bdb服务器程序
> **为何选择bdb，而不直接使用redis:**

> - 数据持久化，不必占用过多服务器内存，可以快速重启服务

> **为何选择bdb，而不直接使用leveldb:**

> - 高可用性，底层直接支持主从复制，主从切换
> - 支持事务
> - 可选购Oracle服务支持
