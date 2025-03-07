---
sidebar_label: 数据订阅
description: "数据订阅与推送服务。写入到 TDengine 中的时序数据能够被自动推送到订阅客户端。"
title: 数据订阅
---

import Tabs from "@theme/Tabs";
import TabItem from "@theme/TabItem";
import Java from "./_sub_java.mdx";
import JavaWS from "./_sub_java_ws.mdx";
import Python from "./_sub_python.mdx";
import Go from "./_sub_go.mdx";
import Rust from "./_sub_rust.mdx";
import Node from "./_sub_node.mdx";
import CSharp from "./_sub_cs.mdx";
import CDemo from "./_sub_c.mdx";

为了帮助应用实时获取写入 TDengine 的数据，或者以事件到达顺序处理数据，TDengine 提供了类似消息队列产品的数据订阅、消费接口。这样在很多场景下，采用 TDengine 的时序数据处理系统不再需要集成消息队列产品，比如 kafka, 从而简化系统设计的复杂度，降低运营维护成本。

与 kafka 一样，你需要定义 *topic*, 但 TDengine 的 *topic* 是基于一个已经存在的超级表、子表或普通表的查询条件，即一个 `SELECT` 语句。你可以使用 SQL 对标签、表名、列、表达式等条件进行过滤，以及对数据进行标量函数与 UDF 计算（不包括数据聚合）。与其他消息队列软件相比，这是 TDengine 数据订阅功能的最大的优势，它提供了更大的灵活性，数据的颗粒度可以由应用随时调整，而且数据的过滤与预处理交给 TDengine，而不是应用完成，有效的减少传输的数据量与应用的复杂度。

消费者订阅 *topic* 后，可以实时获得最新的数据。多个消费者可以组成一个消费者组 (consumer group), 一个消费者组里的多个消费者共享消费进度，便于多线程、分布式地消费数据，提高消费速度。但不同消费者组中的消费者即使消费同一个 topic, 并不共享消费进度。一个消费者可以订阅多个 topic。如果订阅的是超级表，数据可能会分布在多个不同的 vnode 上，也就是多个 shard 上，这样一个消费组里有多个消费者可以提高消费效率。TDengine 的消息队列提供了消息的 ACK 机制，在宕机、重启等复杂环境下确保 at least once 消费。

为了实现上述功能，TDengine 会为 WAL (Write-Ahead-Log) 文件自动创建索引以支持快速随机访问，并提供了灵活可配置的文件切换与保留机制：用户可以按需指定 WAL 文件保留的时间以及大小（详见 create database 语句）。通过以上方式将 WAL 改造成了一个保留事件到达顺序的、可持久化的存储引擎（但由于 TSDB 具有远比 WAL 更高的压缩率，我们不推荐保留太长时间，一般来说，不超过几天）。 对于以 topic 形式创建的查询，TDengine 将对接 WAL 而不是 TSDB 作为其存储引擎。在消费时，TDengine 根据当前消费进度从 WAL 直接读取数据，并使用统一的查询引擎实现过滤、变换等操作，将数据推送给消费者。

下面为关于数据订阅的一些说明，需要对TDengine的架构有一些了解，结合各个语言链接器的接口使用。
- 一个消费组消费同一个topic下的所有数据，不同消费组之间相互独立；
- 一个消费组消费同一个topic所有的vgroup，消费组可由多个消费者组成，但一个vgroup仅被一个消费者消费，如果消费者数量超过了vgroup数量，多余的消费者不消费数据；
- 在服务端每个vgroup仅保存一个offset，每个vgroup的offset是单调递增的，但不一定连续。各个vgroup的offset之间没有关联；
- 每次poll服务端会返回一个结果block，该block属于一个vgroup，可能包含多个wal版本的数据，可以通过 offset 接口获得是该block第一条记录的offset；
- 一个消费组如果从未commit过offset，当其成员消费者重启重新拉取数据时，均从参数auto.offset.reset设定值开始消费；在一个消费者生命周期中，客户端本地记录了最近一次拉取数据的offset，不会拉取重复数据；
- 消费者如果异常终止（没有调用tmq_close），需等约12秒后触发其所属消费组rebalance，该消费者在服务端状态变为LOST，约1天后该消费者自动被删除；正常退出，退出后就会删除消费者；新增消费者，需等约2秒触发rebalance，该消费者在服务端状态变为ready；
- 消费组rebalance会对该组所有ready状态的消费者成员重新进行vgroup分配，消费者仅能对自己负责的vgroup进行assignment/seek/commit/poll操作；
- 消费者可利用 position 获得当前消费的offset，并seek到指定offset，重新消费；
- seek将position指向指定offset，不执行commit操作，一旦seek成功，可poll拉取指定offset及以后的数据；
- seek 操作之前须调用 assignment 接口获取该consumer的vgroup ID和offset范围。seek 操作会检测vgroup ID 和 offset是否合法，如非法将报错；
- position是获取当前的消费位置，是下次要取的位置，不是当前消费到的位置
- commit是提交消费位置，不带参数的话，是提交当前消费位置（下次要取的位置，不是当前消费到的位置），带参数的话，是提交参数里的位置（也即下次退出重启后要取的位置）
- seek是设置consumer消费位置，seek到哪，position就返回哪，都是下次要取的位置
- seek不会影响commit，commit不影响seek，相互独立，两个是不同的概念
- begin接口为wal 第一条数据的offset，end 接口为wal 最后一条数据的offset + 1
- offset接口获取的是记录所在结果block块里的第一条数据的offset，当seek至该offset时，将消费到这个block里的全部数据。参见第四点；
- 由于存在 WAL 过期删除机制，即使seek 操作成功，poll数据时有可能offset已失效。如果poll 的offset 小于 WAL 最小版本号，将会从WAL最小版本号消费；
- 数据订阅是从 WAL 消费数据，如果一些 WAL 文件被基于 WAL 保留策略删除，则已经删除的 WAL 文件中的数据就无法再消费到。需要根据业务需要在创建数据库时合理设置 `WAL_RETENTION_PERIOD` 或 `WAL_RETENTION_SIZE` ，并确保应用及时消费数据，这样才不会产生数据丢失的现象。数据订阅的行为与 Kafka 等广泛使用的消息队列类产品的行为相似；

本文档不对消息队列本身的知识做更多的介绍，如果需要了解，请自行搜索。

从3.2.0.0版本开始，数据订阅支持vnode迁移和分裂。
由于数据订阅依赖wal文件，而在vnode迁移和分裂的过程中，wal并不会同步过去，所以迁移或分裂后，之前没消费完的wal数据后消费不到。所以请保证之前把数据全部消费完后，再进行vnode迁移或分裂，否则，消费会丢失数据。

## 主要数据结构和 API

不同语言下， TMQ 订阅相关的 API 及数据结构如下：

<Tabs defaultValue="java" groupId="lang">
<TabItem value="c" label="C">

```c
    typedef struct tmq_t      tmq_t;
    typedef struct tmq_conf_t tmq_conf_t;
    typedef struct tmq_list_t tmq_list_t;

    typedef void(tmq_commit_cb(tmq_t *tmq, int32_t code, void *param));

    typedef enum tmq_conf_res_t {
        TMQ_CONF_UNKNOWN = -2,
        TMQ_CONF_INVALID = -1,
        TMQ_CONF_OK = 0,
    } tmq_conf_res_t;

    typedef struct tmq_topic_assignment {
        int32_t vgId;
        int64_t currentOffset;
        int64_t begin;
        int64_t end;
    } tmq_topic_assignment;

    DLL_EXPORT tmq_conf_t    *tmq_conf_new();
    DLL_EXPORT tmq_conf_res_t tmq_conf_set(tmq_conf_t *conf, const char *key, const char *value);
    DLL_EXPORT void           tmq_conf_destroy(tmq_conf_t *conf);
    DLL_EXPORT void           tmq_conf_set_auto_commit_cb(tmq_conf_t *conf, tmq_commit_cb *cb, void *param);

    DLL_EXPORT tmq_list_t *tmq_list_new();
    DLL_EXPORT int32_t     tmq_list_append(tmq_list_t *, const char *);
    DLL_EXPORT void        tmq_list_destroy(tmq_list_t *);
    DLL_EXPORT int32_t     tmq_list_get_size(const tmq_list_t *);
    DLL_EXPORT char      **tmq_list_to_c_array(const tmq_list_t *);

    DLL_EXPORT tmq_t    *tmq_consumer_new(tmq_conf_t *conf, char *errstr, int32_t errstrLen);
    DLL_EXPORT int32_t   tmq_subscribe(tmq_t *tmq, const tmq_list_t *topic_list);
    DLL_EXPORT int32_t   tmq_unsubscribe(tmq_t *tmq);
    DLL_EXPORT int32_t   tmq_subscription(tmq_t *tmq, tmq_list_t **topics);
    DLL_EXPORT TAOS_RES *tmq_consumer_poll(tmq_t *tmq, int64_t timeout);
    DLL_EXPORT int32_t   tmq_consumer_close(tmq_t *tmq);
    DLL_EXPORT int32_t   tmq_commit_sync(tmq_t *tmq, const TAOS_RES *msg);
    DLL_EXPORT void      tmq_commit_async(tmq_t *tmq, const TAOS_RES *msg, tmq_commit_cb *cb, void *param);
    DLL_EXPORT int32_t   tmq_commit_offset_sync(tmq_t *tmq, const char *pTopicName, int32_t vgId, int64_t offset);
    DLL_EXPORT void      tmq_commit_offset_async(tmq_t *tmq, const char *pTopicName, int32_t vgId, int64_t offset, tmq_commit_cb *cb, void *param);
    DLL_EXPORT int32_t   tmq_get_topic_assignment(tmq_t *tmq, const char *pTopicName, tmq_topic_assignment **assignment,int32_t *numOfAssignment);
    DLL_EXPORT void      tmq_free_assignment(tmq_topic_assignment* pAssignment);
    DLL_EXPORT int32_t   tmq_offset_seek(tmq_t *tmq, const char *pTopicName, int32_t vgId, int64_t offset);
    DLL_EXPORT int64_t   tmq_position(tmq_t *tmq, const char *pTopicName, int32_t vgId);
    DLL_EXPORT int64_t   tmq_committed(tmq_t *tmq, const char *pTopicName, int32_t vgId);

    DLL_EXPORT const char *tmq_get_topic_name(TAOS_RES *res);
    DLL_EXPORT const char *tmq_get_db_name(TAOS_RES *res);
    DLL_EXPORT int32_t     tmq_get_vgroup_id(TAOS_RES *res);
    DLL_EXPORT int64_t     tmq_get_vgroup_offset(TAOS_RES* res);
    DLL_EXPORT const char *tmq_err2str(int32_t code);
```

下面介绍一下它们的具体用法（超级表和子表结构请参考“数据建模”一节），完整的示例代码请见下面 C 语言的示例代码。

</TabItem>
<TabItem value="java" label="Java">

```java
void subscribe(Collection<String> topics) throws SQLException;

void unsubscribe() throws SQLException;

Set<String> subscription() throws SQLException;

ConsumerRecords<V> poll(Duration timeout) throws SQLException;

Set<TopicPartition> assignment() throws SQLException;
long position(TopicPartition partition) throws SQLException;
Map<TopicPartition, Long> position(String topic) throws SQLException;
Map<TopicPartition, Long> beginningOffsets(String topic) throws SQLException;
Map<TopicPartition, Long> endOffsets(String topic) throws SQLException;
Map<TopicPartition, OffsetAndMetadata> committed(Set<TopicPartition> partitions) throws SQLException;

void seek(TopicPartition partition, long offset) throws SQLException;
void seekToBeginning(Collection<TopicPartition> partitions) throws SQLException;
void seekToEnd(Collection<TopicPartition> partitions) throws SQLException;

void commitSync() throws SQLException;
void commitSync(Map<TopicPartition, OffsetAndMetadata> offsets) throws SQLException;

void close() throws SQLException;
```

</TabItem>

<TabItem value="Python" label="Python">

```python
class Consumer:
    def subscribe(self, topics):
        pass

    def unsubscribe(self):
        pass

    def poll(self, timeout: float = 1.0):
        pass

    def assignment(self):
        pass

    def seek(self, partition):
        pass

    def close(self):
        pass

    def commit(self, message):
        pass
```

</TabItem>

<TabItem label="Go" value="Go">

```go
func NewConsumer(conf *tmq.ConfigMap) (*Consumer, error)

// 出于兼容目的保留 rebalanceCb 参数，当前未使用
func (c *Consumer) Subscribe(topic string, rebalanceCb RebalanceCb) error

// 出于兼容目的保留 rebalanceCb 参数，当前未使用
func (c *Consumer) SubscribeTopics(topics []string, rebalanceCb RebalanceCb) error

func (c *Consumer) Poll(timeoutMs int) tmq.Event

// 出于兼容目的保留 tmq.TopicPartition 参数，当前未使用
func (c *Consumer) Commit() ([]tmq.TopicPartition, error)

func (c *Consumer) Unsubscribe() error

func (c *Consumer) Close() error
```

</TabItem>

<TabItem label="Rust" value="Rust">

```rust
impl TBuilder for TmqBuilder
  fn from_dsn<D: IntoDsn>(dsn: D) -> Result<Self, Self::Error>
  fn build(&self) -> Result<Self::Target, Self::Error>

impl AsAsyncConsumer for Consumer
  async fn subscribe<T: Into<String>, I: IntoIterator<Item = T> + Send>(
        &mut self,
        topics: I,
    ) -> Result<(), Self::Error>;
  fn stream(
        &self,
    ) -> Pin<
        Box<
            dyn '_
                + Send
                + futures::Stream<
                    Item = Result<(Self::Offset, MessageSet<Self::Meta, Self::Data>), Self::Error>,
                >,
        >,
    >;
  async fn commit(&self, offset: Self::Offset) -> Result<(), Self::Error>;

  async fn unsubscribe(self);
```

可在 <https://docs.rs/taos> 上查看详细 API 说明。

</TabItem>

<TabItem label="Node.JS" value="Node.JS">

```js
function TMQConsumer(config)

function subscribe(topic)

function consume(timeout)

function subscription()

function unsubscribe()

function commit(msg)

function close()
```

</TabItem>

<TabItem value="C#" label="C#">

```csharp
ConsumerBuilder(IEnumerable<KeyValuePair<string, string>> config)

virtual IConsumer Build()

Consumer(ConsumerBuilder builder)

void Subscribe(IEnumerable<string> topics)

void Subscribe(string topic) 

ConsumeResult Consume(int millisecondsTimeout)

List<string> Subscription()

void Unsubscribe()
 
void Commit(ConsumeResult consumerResult)

void Close()
```

</TabItem>
</Tabs>

## 写入数据

首先完成建库、建一张超级表和多张子表操作，然后就可以写入数据了，比如：

```sql
DROP DATABASE IF EXISTS tmqdb;
CREATE DATABASE tmqdb WAL_RETENTION_PERIOD 3600;
CREATE TABLE tmqdb.stb (ts TIMESTAMP, c1 INT, c2 FLOAT, c3 VARCHAR(16)) TAGS(t1 INT, t3 VARCHAR(16));
CREATE TABLE tmqdb.ctb0 USING tmqdb.stb TAGS(0, "subtable0");
CREATE TABLE tmqdb.ctb1 USING tmqdb.stb TAGS(1, "subtable1");       
INSERT INTO tmqdb.ctb0 VALUES(now, 0, 0, 'a0')(now+1s, 0, 0, 'a00');
INSERT INTO tmqdb.ctb1 VALUES(now, 1, 1, 'a1')(now+1s, 11, 11, 'a11');
```

## 创建 *topic*

TDengine 使用 SQL 创建一个 topic：

```sql
CREATE TOPIC topic_name AS SELECT ts, c1, c2, c3 FROM tmqdb.stb WHERE c1 > 1;
```
- topic创建个数有上限，通过参数 tmqMaxTopicNum 控制，默认 20 个

TMQ 支持多种订阅类型：

### 列订阅

语法：

```sql
CREATE TOPIC topic_name as subquery
```

通过 `SELECT` 语句订阅（包括 `SELECT *`，或 `SELECT ts, c1` 等指定列订阅，可以带条件过滤、标量函数计算，但不支持聚合函数、不支持时间窗口聚合）。需要注意的是：

- 该类型 TOPIC 一旦创建则订阅数据的结构确定。
- 被订阅或用于计算的列或标签不可被删除（`ALTER table DROP`）、修改（`ALTER table MODIFY`）。
- 若发生表结构变更，新增的列不出现在结果中。

### 超级表订阅

语法：

```sql
CREATE TOPIC topic_name [with meta] AS STABLE stb_name [where_condition]
```

与 `SELECT * from stbName` 订阅的区别是：

- 不会限制用户的表结构变更。
- 返回的是非结构化的数据：返回数据的结构会随之超级表的表结构变化而变化。
- with meta 参数可选，选择时将返回创建超级表，子表等语句，主要用于taosx做超级表迁移
- where_condition 参数可选，选择时将用来过滤符合条件的子表，订阅这些子表。where 条件里不能有普通列，只能是tag或tbname，where条件里可以用函数，用来过滤tag，但是不能是聚合函数，因为子表tag值无法做聚合。也可以是常量表达式，比如 2 > 1（订阅全部子表），或者 false（订阅0个子表）
- 返回数据不包含标签。

### 数据库订阅

语法：

```sql
CREATE TOPIC topic_name [with meta] AS DATABASE db_name;
```

通过该语句可创建一个包含数据库所有表数据的订阅

- with meta 参数可选，选择时将返回创建数据库里所有超级表，子表的语句，主要用于taosx做数据库迁移

## 创建消费者 *consumer*

消费者需要通过一系列配置选项创建，基础配置项如下表所示：

|            参数名称            |  类型   | 参数说明                                                 | 备注                                        |
| :----------------------------: | :-----: | -------------------------------------------------------- | ------------------------------------------- |
|        `td.connect.ip`         | string  | 服务端的 IP 地址                          |                                          |
|       `td.connect.user`        | string  | 用户名                         |    |
|       `td.connect.pass`        | string  | 密码                          |  |
|       `td.connect.port`        | integer | 服务端的端口号                         |  |
|           `group.id`           | string  | 消费组 ID，同一消费组共享消费进度                        | <br />**必填项**。最大长度：192。<br />每个topic最多可建立100个 consumer group                 |
|          `client.id`           | string  | 客户端 ID                                                | 最大长度：192。                             |
|      `auto.offset.reset`       |  enum   | 消费组订阅的初始位置                                     | <br />`earliest`: default(version < 3.2.0.0);从头开始订阅; <br/>`latest`: default(version >= 3.2.0.0);仅从最新数据开始订阅; <br/>`none`: 没有提交的 offset 无法订阅 |
|      `enable.auto.commit`      | boolean | 是否启用消费位点自动提交，true: 自动提交，客户端应用无需commit；false：客户端应用需要自行commit     | 默认值为 true                   |
|   `auto.commit.interval.ms`    | integer | 消费记录自动提交消费位点时间间隔，单位为毫秒           | 默认值为 5000                                |
|     `msg.with.table.name`      | boolean | 是否允许从消息中解析表名, 不适用于列订阅（列订阅时可将 tbname 作为列写入 subquery 语句）（从3.2.0.0版本该参数废弃，恒为true）               |默认关闭 |

对于不同编程语言，其设置方式如下：

<Tabs defaultValue="java" groupId="lang">
<TabItem value="c" label="C">

```c
/* 根据需要，设置消费组 (group.id)、自动提交 (enable.auto.commit)、
   自动提交时间间隔 (auto.commit.interval.ms)、用户名 (td.connect.user)、密码 (td.connect.pass) 等参数 */
tmq_conf_t* conf = tmq_conf_new();
tmq_conf_set(conf, "enable.auto.commit", "true");
tmq_conf_set(conf, "auto.commit.interval.ms", "1000");
tmq_conf_set(conf, "group.id", "cgrpName");
tmq_conf_set(conf, "td.connect.user", "root");
tmq_conf_set(conf, "td.connect.pass", "taosdata");
tmq_conf_set(conf, "auto.offset.reset", "earliest");
tmq_conf_set(conf, "msg.with.table.name", "true");
tmq_conf_set_auto_commit_cb(conf, tmq_commit_cb_print, NULL);

tmq_t* tmq = tmq_consumer_new(conf, NULL, 0);
tmq_conf_destroy(conf);
```

</TabItem>
<TabItem value="java" label="Java">

对于 Java 程序，还可以使用如下配置项：

| 参数名称                      | 类型   | 参数说明                                                                                                                      |
| ----------------------------- | ------ | ----------------------------------------------------------------------------------------------------------------------------- |
| `td.connect.type` | string | 连接类型，"jni" 指原生连接，"ws" 指 websocket 连接，默认值为 "jni" |
| `bootstrap.servers`           | string | 连接地址，如 `localhost:6030`                                                                                                 |
| `value.deserializer`          | string | 值解析方法，使用此方法应实现 `com.taosdata.jdbc.tmq.Deserializer` 接口或继承 `com.taosdata.jdbc.tmq.ReferenceDeserializer` 类 |
| `value.deserializer.encoding` | string | 指定字符串解析的字符集                                                                                                        |  |

需要注意：此处使用 `bootstrap.servers` 替代 `td.connect.ip` 和 `td.connect.port`，以提供与 Kafka 一致的接口。

```java
Properties properties = new Properties();
properties.setProperty("enable.auto.commit", "true");
properties.setProperty("auto.commit.interval.ms", "1000");
properties.setProperty("group.id", "cgrpName");
properties.setProperty("bootstrap.servers", "127.0.0.1:6030");
properties.setProperty("td.connect.user", "root");
properties.setProperty("td.connect.pass", "taosdata");
properties.setProperty("auto.offset.reset", "earliest");
properties.setProperty("msg.with.table.name", "true");
properties.setProperty("value.deserializer", "com.taos.example.MetersDeserializer");

TaosConsumer<Meters> consumer = new TaosConsumer<>(properties);

/* value deserializer definition. */
import com.taosdata.jdbc.tmq.ReferenceDeserializer;

public class MetersDeserializer extends ReferenceDeserializer<Meters> {
}
```

</TabItem>

<TabItem label="Go" value="Go">

```go
conf := &tmq.ConfigMap{
 "group.id":                     "test",
 "auto.offset.reset":            "earliest",
 "td.connect.ip":                "127.0.0.1",
 "td.connect.user":              "root",
 "td.connect.pass":              "taosdata",
 "td.connect.port":              "6030",
 "client.id":                    "test_tmq_c",
 "enable.auto.commit":           "false",
 "msg.with.table.name":          "true",
}
consumer, err := NewConsumer(conf)
```

</TabItem>

<TabItem label="Rust" value="Rust">

```rust
let mut dsn: Dsn = "taos://".parse()?;
dsn.set("group.id", "group1");
dsn.set("client.id", "test");
dsn.set("auto.offset.reset", "earliest");

let tmq = TmqBuilder::from_dsn(dsn)?;

let mut consumer = tmq.build()?;
```

</TabItem>

<TabItem value="Python" label="Python">

Python 语言下引入 `taos` 库的 `Consumer` 类，创建一个 Consumer 示例：

```python
from taos.tmq import Consumer

# Syntax: `consumer = Consumer(configs)`
#
# Example:
consumer = Consumer(
    {
        "group.id": "local",
        "client.id": "1",
        "enable.auto.commit": "true",
        "auto.commit.interval.ms": "1000",
        "td.connect.ip": "127.0.0.1",
        "td.connect.user": "root",
        "td.connect.pass": "taosdata",
        "auto.offset.reset": "earliest",
        "msg.with.table.name": "true",
    }
)
```

</TabItem>

<TabItem label="Node.JS" value="Node.JS">

```js
// 根据需要，设置消费组 (group.id)、自动提交 (enable.auto.commit)、
// 自动提交时间间隔 (auto.commit.interval.ms)、用户名 (td.connect.user)、密码 (td.connect.pass) 等参数 

let consumer = taos.consumer({
  'enable.auto.commit': 'true',
  'auto.commit.interval.ms','1000',
  'group.id': 'tg2',
  'td.connect.user': 'root',
  'td.connect.pass': 'taosdata',
  'auto.offset.reset','earliest',
  'msg.with.table.name': 'true',
  'td.connect.ip','127.0.0.1',
  'td.connect.port','6030'  
  });
```

</TabItem>

<TabItem value="C#" label="C#">

```csharp
using TDengineTMQ;

// 根据需要，设置消费组 (GourpId)、自动提交 (EnableAutoCommit)、
// 自动提交时间间隔 (AutoCommitIntervalMs)、用户名 (TDConnectUser)、密码 (TDConnectPasswd) 等参数
var cfg = new ConsumerConfig
 {
    EnableAutoCommit = "true"
    AutoCommitIntervalMs = "1000"
    GourpId = "TDengine-TMQ-C#",
    TDConnectUser = "root",
    TDConnectPasswd = "taosdata",
    AutoOffsetReset = "earliest"
    MsgWithTableName = "true",
    TDConnectIp = "127.0.0.1",
    TDConnectPort = "6030"
 };

var consumer = new ConsumerBuilder(cfg).Build();

```

</TabItem>

</Tabs>

上述配置中包括 consumer group ID，如果多个 consumer 指定的 consumer group ID 一样，则自动形成一个 consumer group，共享消费进度。

## 订阅 *topics*

一个 consumer 支持同时订阅多个 topic。

<Tabs defaultValue="java" groupId="lang">
<TabItem value="c" label="C">

```c
// 创建订阅 topics 列表
tmq_list_t* topicList = tmq_list_new();
tmq_list_append(topicList, "topicName");
// 启动订阅
tmq_subscribe(tmq, topicList);
tmq_list_destroy(topicList);
  
```

</TabItem>
<TabItem value="java" label="Java">

```java
List<String> topics = new ArrayList<>();
topics.add("tmq_topic");
consumer.subscribe(topics);
```

</TabItem>
<TabItem value="Go" label="Go">

```go
err = consumer.Subscribe("example_tmq_topic", nil)
if err != nil {
 panic(err)
}
```

</TabItem>
<TabItem value="Rust" label="Rust">

```rust
consumer.subscribe(["tmq_meters"]).await?;
```

</TabItem>

<TabItem value="Python" label="Python">

```python
consumer.subscribe(['topic1', 'topic2'])
```

</TabItem>

<TabItem label="Node.JS" value="Node.JS">

```js
// 创建订阅 topics 列表
let topics = ['topic_test']

// 启动订阅
consumer.subscribe(topics);
```

</TabItem>

<TabItem value="C#" label="C#">

```csharp
// 创建订阅 topics 列表
List<String> topics = new List<string>();
topics.add("tmq_topic");
// 启动订阅
consumer.Subscribe(topics);
```

</TabItem>

</Tabs>

## 消费

以下代码展示了不同语言下如何对 TMQ 消息进行消费。

<Tabs defaultValue="java" groupId="lang">
<TabItem value="c" label="C">

```c
// 消费数据
while (running) {
  TAOS_RES* msg = tmq_consumer_poll(tmq, timeOut);
  msg_process(msg);
}  
```

这里是一个 **while** 循环，每调用一次 tmq_consumer_poll()，获取一个消息，该消息与普通查询返回的结果集完全相同，可以使用相同的解析 API 完成消息内容的解析。

</TabItem>
<TabItem value="java" label="Java">

```java
while(running){
  ConsumerRecords<Meters> meters = consumer.poll(Duration.ofMillis(100));
    for (Meters meter : meters) {
      processMsg(meter);
    }    
}
```

</TabItem>

<TabItem value="Go" label="Go">

```go
for {
 ev := consumer.Poll(0)
 if ev != nil {
  switch e := ev.(type) {
  case *tmqcommon.DataMessage:
   fmt.Println(e.Value())
  case tmqcommon.Error:
   fmt.Fprintf(os.Stderr, "%% Error: %v: %v\n", e.Code(), e)
   panic(e)
  }
  consumer.Commit()
 }
}
```

</TabItem>

<TabItem value="Rust" label="Rust">

```rust
{
    let mut stream = consumer.stream();

    while let Some((offset, message)) = stream.try_next().await? {
        // get information from offset

        // the topic
        let topic = offset.topic();
        // the vgroup id, like partition id in kafka.
        let vgroup_id = offset.vgroup_id();
        println!("* in vgroup id {vgroup_id} of topic {topic}\n");

        if let Some(data) = message.into_data() {
            while let Some(block) = data.fetch_raw_block().await? {
                // one block for one table, get table name if needed
                let name = block.table_name();
                let records: Vec<Record> = block.deserialize().try_collect()?;
                println!(
                    "** table: {}, got {} records: {:#?}\n",
                    name.unwrap(),
                    records.len(),
                    records
                );
            }
        }
        consumer.commit(offset).await?;
    }
}
```

</TabItem>
<TabItem value="Python" label="Python">

```python
while True:
    res = consumer.poll(100)
    if not res:
        continue
    err = res.error()
    if err is not None:
        raise err
    val = res.value()

    for block in val:
        print(block.fetchall())
```

</TabItem>

<TabItem label="Node.JS" value="Node.JS">

```js
while(true){
  msg = consumer.consume(200);
  // process message(consumeResult)
  console.log(msg.topicPartition);
  console.log(msg.block);
  console.log(msg.fields)
}
```

</TabItem>

<TabItem value="C#" label="C#">

```csharp
// 消费数据
while (true)
{
    var consumerRes = consumer.Consume(100);
    // process ConsumeResult
    ProcessMsg(consumerRes);
    consumer.Commit(consumerRes);
}
```

</TabItem>

</Tabs>

## 结束消费

消费结束后，应当取消订阅。

<Tabs defaultValue="java" groupId="lang">
<TabItem value="c" label="C">

```c
/* 取消订阅 */
tmq_unsubscribe(tmq);

/* 关闭消费者对象 */
tmq_consumer_close(tmq);
```

</TabItem>
<TabItem value="java" label="Java">

```java
/* 取消订阅 */
consumer.unsubscribe();

/* 关闭消费 */
consumer.close();
```

</TabItem>

<TabItem value="Go" label="Go">

```go
/* Unsubscribe */
_ = consumer.Unsubscribe()

/* Close consumer */
_ = consumer.Close()
```

</TabItem>

<TabItem value="Rust" label="Rust">

```rust
consumer.unsubscribe().await;
```

</TabItem>

<TabItem value="Python" label="Python">

```py
# 取消订阅
consumer.unsubscribe()
# 关闭消费
consumer.close()
```

</TabItem>
<TabItem label="Node.JS" value="Node.JS">

```js
consumer.unsubscribe();
consumer.close();
```

</TabItem>

<TabItem value="C#" label="C#">

```csharp
// 取消订阅
consumer.Unsubscribe();

// 关闭消费
consumer.Close();
```

</TabItem>

</Tabs>

## 删除 *topic*

如果不再需要订阅数据，可以删除 topic，需要注意：只有当前未在订阅中的 TOPIC 才能被删除。

```sql
/* 删除 topic */
DROP TOPIC topic_name;
```

## 状态查看

1、*topics*：查询已经创建的 topic

```sql
SHOW TOPICS;
```

2、consumers：查询 consumer 的状态及其订阅的 topic

```sql
SHOW CONSUMERS;
```

3、subscriptions：查询 consumer 与 vgroup 之间的分配关系

```sql
SHOW SUBSCRIPTIONS;
```

## 示例代码

以下是各语言的完整示例代码。

<Tabs defaultValue="java" groupId="lang">

<TabItem label="C" value="c">
  <CDemo />
</TabItem>

<TabItem label="Java" value="java">
<Tabs defaultValue="native">
<TabItem value="native" label="本地连接">
<Java />
</TabItem>
<TabItem value="ws" label="WebSocket 连接">
<JavaWS />
</TabItem>
</Tabs>
</TabItem>

<TabItem label="Go" value="Go">
   <Go/>
</TabItem>

<TabItem label="Rust" value="Rust">
    <Rust />
</TabItem>

<TabItem label="Python" value="Python">
    <Python />
</TabItem>

<TabItem label="Node.JS" value="Node.JS">
   <Node/>
</TabItem>

<TabItem label="C#" value="C#">
   <CSharp/>
</TabItem>

</Tabs>
