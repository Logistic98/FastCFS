# FastCFS安装手册

## 1、fastcfs.sh 脚本统一安装

通过执行fastcfs.sh脚本，可自动安装软件包，并能根据配置文件模版自动生成集群相关配置文件。

fastcfs.sh 命令参数说明：

```
* install: 安装程序包
* config: 复制配置文件并自动配置为单节点
* start | stop | restart: 服务进程控制
```

或执行如下命令（需要root身份执行）：

```
./fastcfs.sh install
./fastcfs.sh config
./fastcfs.sh restart
```

通过 df 命令查看FastCFS挂载的文件目录：

```
df -h /opt/fastcfs/fuse | grep fuse
```

以上命令执行成功，你就可以通过本地目录 /opt/fastcfs/fuse 访问FastCFS了。

## 2、DIY编译和安装

### 2.1 安装libaio devel包

对于 CentOS或REHL，执行：

```
yum install libaio-devel -y
```

对于 Unbuntu或Debian，执行：

```
apt install libaio-dev -y
```

其他Linux系统，需要编译安装 **libaio** 库。

### 2.2 安装存储插件包和RDMA通信包【可选】

fastDIR 和faststore 的存储插件以及RDMA通信库都是闭源的，可以通过 yum 或 apt 安装使用。

配置我们的yum源和apt源：

* yum安装方式（针对CentOS、Rocky、Fedora、RHEL等），参阅 [yum安装文档](YUM-INSTALL-zh_CN.md)
* apt安装方式（针对Ubuntu、Debian 和 Deepin），参阅 [apt 安装文档](APT-INSTALL-zh_CN.md)

如果使用存储插件特性，需要通过 yum 或 apt 安装相应的存储插件。

* fastDIR 存储插件包名：libfdirstorage
* faststore 存储插件包名：libfsstorage

具备RDMA网络环境，要启动 RDMA 通信方式，需要安装RDMA通信包：libfastrdma


### 2.3 libfastcommon 编译安装

```
git clone https://gitee.com/fastdfs100/libfastcommon.git; cd libfastcommon/
git checkout master
./make.sh clean && ./make.sh && ./make.sh install
```

默认安装目录：
```
/usr/lib64
/usr/lib
/usr/include/fastcommon
```

### 2.4 libserverframe 编译安装

```
git clone https://gitee.com/fastdfs100/libserverframe.git; cd libserverframe/
./make.sh clean && ./make.sh && ./make.sh install
```

### 2.5 libdiskallocator 编译安装

```
git clone https://gitee.com/fastdfs100/libdiskallocator.git; cd libdiskallocator/
./make.sh clean && ./make.sh && ./make.sh install
```

### 2.6 Vote Node client 编译安装

```
git clone https://gitee.com/fastdfs100/FastCFS.git; cd FastCFS/
./make.sh clean && ./make.sh --module=voteclient && ./make.sh --module=voteclient install
```

### 2.7 Auth client 编译安装

```
git clone https://gitee.com/fastdfs100/FastCFS.git; cd FastCFS/
./make.sh clean && ./make.sh --module=authclient && ./make.sh --module=authclient install
```

### 2.8 fastDIR 编译安装

```
git clone https://gitee.com/fastdfs100/fastDIR.git; cd fastDIR/
./make.sh clean && ./make.sh && ./make.sh install
mkdir -p /etc/fastcfs/fdir/
cp conf/*.conf /etc/fastcfs/fdir/
```

### 2.9 faststore 编译安装

```
git clone https://gitee.com/fastdfs100/faststore.git; cd faststore/
./make.sh clean && ./make.sh && ./make.sh install
mkdir -p /etc/fastcfs/fstore/
cp conf/*.conf /etc/fastcfs/fstore/
```

### 2.10 fuse客户端编译安装

#### 2.10.1 libfuse 编译安装

libfuse 编译依赖比较复杂，建议使用脚本libfuse_setup.sh一键编译和安装。或者执行如下步骤DIY：

构建libfuse需要先安装meson和ninja。安装meson和ninja需要python3.5及更高版本。

##### 2.10.1.1 gcc 安装

友情提示：gcc版本必须 >= 4.7.0

Ubuntu下安装命令：

```
apt install gcc g++ -y
```

CentOS下安装命令：

```
yum install gcc gcc-c++ -y
```

##### 2.10.1.2 pkg-config 和 python安装

需要安装的包名：
* pkg-config
* python3
* python3-pip

Ubuntu下安装命令：

```
apt install pkg-config python3 python3-pip -y
```

CentOS下安装命令：

```
yum install pkgconfig python3 python3-pip -y
```

##### 2.10.1.3 meson 和 ninja 安装

友情提示： ninja 版本必须 >= 1.7

```
pip3 install meson
pip3 install ninja
```

##### 2.10.1.4 libfuse 安装

```
git clone https://gitee.com/mirrors/libfuse.git; cd libfuse/
git checkout fuse-3.16.2
mkdir build/; cd build/
meson ..
meson configure -D prefix=/usr
meson configure -D examples=false
ninja && ninja install
sed -i 's/#user_allow_other/user_allow_other/g' /etc/fuse.conf
sed -i "s#prefix=.*#prefix=/usr#g" /usr/lib64/pkgconfig/fuse3.pc
```

#### 2.10.2 依赖的项目编译和安装

参见上面的2.3 ～2.8，按照步骤逐一编译和安装


#### 2.10.3 fcfs_fused 编译安装

进入之前clone下来的FastCFS目录，然后执行：
```
./make.sh --module=fuseclient clean && ./make.sh --module=fuseclient && ./make.sh --module=fuseclient install
mkdir -p /etc/fastcfs/fcfs/
mkdir -p /etc/fastcfs/vote/
mkdir -p /etc/fastcfs/auth/
cp conf/*.conf /etc/fastcfs/fcfs/
cp -R src/vote/conf/* /etc/fastcfs/vote/
cp -R src/auth/conf/* /etc/fastcfs/auth/
```

配置参见 [配置文档](CONFIGURE-zh_CN.md)
