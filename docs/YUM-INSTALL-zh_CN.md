
## yum安装方式

yum 安装方式支持intel的x86_64和ARM的aarch64架构，主要用于测试和生产环境搭建。
支持的Linux发行版如下：
* CentOS & CentOS Stream
* Fedora
* Rocky
* Anolis
* AlmaLinux
* RHEL (Red Hat Enterprise Linux)
* Oracle Linux
* Amazon Linux
* Alibaba Cloud Linux
* openEuler
* Kylin
* UOS

### 1. 安装FastOS.repo

先安装FastOS.repo yum源，然后就可以安装FastCFS相关软件包了。

CentOS 7、RHEL 7、Oracle Linux 7、Alibaba Cloud Linux 2、Anolis 7、AlmaLinux 7、Amazon Linux 2、Fedora 27及以下版本：
```
rpm -ivh http://www.fastken.cn/yumrepo/el7/noarch/FastOSrepo-1.0.1-1.el7.noarch.rpm
```

CentOS 8、Rocky 8、RHEL 8、Oracle Linux 8、Alibaba Cloud Linux 3、Anolis 8、AlmaLinux 8、openEuler 20.03、Kylin V10、UOS 20、Amazon Linux 3、Fedora 28及以上版本：
```
rpm -ivh http://www.fastken.cn/yumrepo/el8/noarch/FastOSrepo-1.0.1-1.el8.noarch.rpm
```

### 2. fastDIR server安装

在需要运行 fastDIR server的服务器上执行：
```
yum install fastDIR-server -y
```

### 3. faststore server安装

在需要运行 faststore server的服务器上执行：
```
yum install faststore-server -y
```

### 4. FastCFS客户端安装

在需要使用FastCFS存储服务的机器（即FastCFS客户端）上，除了openEuler之外的其他Linux发行版执行：
```
yum install FastCFS-fused -y
```

对于openEuler 20.03和Kylin V10，因需安装的fuse3和系统已有的fuse在依赖关系上存在冲突，需要用rpm命令强制安装fuse3，执行：
```
arch=$(uname -r | awk -F '.' '{print $NF}');
dist=el8;
ver='3.16.2-2';
rpm -ivh http://www.fastken.cn/yumrepo/$dist/$arch/fuse3-libs-$ver.$dist.$arch.rpm  \
         http://www.fastken.cn/yumrepo/$dist/$arch/fuse-common-$ver.$dist.$arch.rpm \
         http://www.fastken.cn/yumrepo/$dist/$arch/fuse3-$ver.$dist.$arch.rpm --force --nodeps;

yum install FastCFS-fused -y
```

对于openEuler 22.03及更高版本，直接执行：
```
yum install FastCFS-fused -y
```

### 5. Vote server安装（可选）

Vote server作为FastCFS多个服务模块共用的选举节点，主要作用是实现双副本防脑裂（即双活互备防脑裂）。

在需要运行选举节点的服务器上执行：
```
yum install FastCFS-vote-server -y
```

### 6. Auth server安装（可选）

如果需要存储池或访问控制，则需要安装本模块。

在需要运行 Auth server的服务器上执行：
```
yum install FastCFS-auth-server -y
```

安装完成后，需要修改对应的配置文件，服务才可以正常启动。请参阅[配置指南](CONFIGURE-zh_CN.md)

FastCFS后台程序可通过systemd管理。systemd服务名称如下：
```
  * fastdir： 目录服务，对应程序为 fdir_serverd
  * faststore：存储服务，对应程序为 fs_serverd
  * fastcfs： FUSE后台服务，对应程序为 fcfs_fused
  * fastvote： 选举节点，对应程序为 fcfs_voted
  * fastauth： 认证服务，对应程序为 fcfs_authd
```

可以使用标准的systemd命令对上述5个服务进行管理，例如：
```
systemctl restart fastdir
systemctl restart faststore
systemctl restart fastcfs
systemctl restart fastvote
systemctl restart fastauth
```
### 6. 一个包含5个节点的安装过程的详细文档
请参与[5节点安装文档](YUM-INSTALL-Diy-5nodes-zh_CN.md)
