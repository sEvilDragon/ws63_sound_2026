# http_utils 重写教程

## 1. 这份文档的目标

你现在这组函数表面上叫做 HTTP 工具，但从实际实现和调用场景来看，它们并不是一个完整的 HTTP 库，而是下面几类能力的混合体：

- C 风格字符串安全处理
- ASCII 文本规整与大小写无关匹配
- 简化版 HTTP URL 解析
- IPv4 地址解析
- 简化版 HTTP 头字段提取
- 简化版 XML 标签内容提取
- 极少量 HTML 实体处理

它们目前能工作，是因为调用方场景比较固定：

- miniMP3 里主要拿它们做 HTTP 拉流
- dlan 里主要拿它们做 SSDP、SUBSCRIBE、SOAP 请求处理

所以这组代码的真实定位更像是：

> 为当前嵌入式 DLNA/HTTP 音频播放场景服务的一组“轻量字符串扫描工具”。

如果你准备“重新写这些内容”，建议不要简单照抄函数名和行为，而是先把能力拆层，再决定每层暴露什么接口。

---

## 2. 先看全局：这组函数到底分成哪几类

当前函数列表：

```cpp
void copy_string_safe(char *dst, size_t dst_size, const char *src);
void html_entity_decode_amp(char *text);
void strip_angle_brackets(char *text);
bool parse_http_url(const char *url, simple_http_url &out);
bool resolve_ipv4_addr(const char *host, in_addr *out_addr);
bool extract_http_header_value(const char *request, const char *key, char *out, size_t out_size);
void trim_ascii_whitespace(char *text);
bool ascii_iequals(const char *a, const char *b);
bool ascii_icontains(const char *haystack, const char *needle);
char *extract_xml_tag_value(const char *buffer, const char *tag, char *out, size_t out_size);
```

更合理的分类应该是：

### 2.1 文本基础层

- copy_string_safe
- trim_ascii_whitespace
- ascii_iequals
- ascii_icontains
- strip_angle_brackets

### 2.2 协议文本扫描层

- extract_http_header_value
- extract_xml_tag_value

### 2.3 URL / 网络层

- parse_http_url
- resolve_ipv4_addr

### 2.4 文本解码层

- html_entity_decode_amp

也就是说，当前最大的结构问题不是“函数写得对不对”，而是：

- 命名偏业务化
- 某些函数过于具体，只适合一个场景
- 某些函数其实是更底层的通用能力，却没有被抽象出来
- 返回值和错误信息过少，不利于重写后的调试和扩展

---

## 3. 当前实现的共同假设

重写前必须先看清楚这些函数依赖了哪些隐含前提：

### 3.1 全部基于 C 风格字符串

几乎所有函数都假设输入满足：

- 以 `\0` 结尾
- 调用者负责缓冲区分配
- 调用者知道目标缓冲区大小

这会带来两个结果：

- 好处是简单、轻量、适合 MCU
- 坏处是截断、越界、空字符串、空指针处理都要靠调用者保持纪律

### 3.2 多数函数是“原地修改”

例如：

- html_entity_decode_amp
- strip_angle_brackets
- trim_ascii_whitespace

这意味着：

- 输入缓冲区必须可写
- 不能直接对字符串字面量调用
- 调试时要特别注意修改前后状态

### 3.3 不是完整协议解析器，只是“够用的文本扫描器”

例如：

- parse_http_url 只支持 `http://`
- extract_http_header_value 只扫描简单头字段
- extract_xml_tag_value 根本不是真 XML 解析器

如果以后场景稍微变化，比如：

- 出现 https
- 出现 IPv6
- XML 标签带属性
- 响应头有重复字段
- 头字段值里需要更复杂的 token 解析

那当前实现就会很快碰到边界。

---

## 4. 函数依赖关系

当前这几个函数之间只有很轻的依赖：

```text
trim_ascii_whitespace
        ↑
strip_angle_brackets

copy_string_safe
        ↑
parse_http_url
```

其余函数基本互相独立。

这说明重写时非常适合按模块拆开，而不是继续堆在一个头文件里。

---

## 5. 逐个函数拆解

## 5.1 copy_string_safe

当前签名：

```cpp
void copy_string_safe(char *dst, size_t dst_size, const char *src);
```

### 它现在做了什么

- 如果 `dst == nullptr` 或 `dst_size == 0`，直接返回
- 如果 `src == nullptr`，把目标写成空串
- 否则用 `snprintf(dst, dst_size, "%s", src)` 复制字符串

### 它的核心价值

这是一个“带目标容量限制的 C 字符串复制”。

本质用途不是 HTTP，而是：

- 防止目标缓冲区溢出
- 保证目标最终有 `\0`
- 统一处理 `src == nullptr`

### 它的优点

- 足够简单
- 行为稳定
- 适合 MCU 场景

### 它的问题

- 返回值是 `void`，调用者无法知道是否发生截断
- 调用者无法知道复制了多少字符
- `snprintf` 用于普通复制时稍显笨重
- 名字里带 `safe`，但并没有把“截断”暴露出来，严格说只是“尽量不越界”

### 更通用的抽象方式

你真正需要的能力应该叫：

- 有界字符串复制
- 有界字符串追加
- 是否截断可观察

建议改成下面这种风格：

```cpp
enum class copy_result {
    ok,
    truncated,
    invalid_arg,
};

copy_result copy_cstr(char *dst, size_t dst_size, const char *src, size_t *written = nullptr);
copy_result append_cstr(char *dst, size_t dst_size, const char *src, size_t *written = nullptr);
```

### 重写建议

- 如果只是复制，不必用 `snprintf`
- 直接按字符复制到 `dst_size - 1`，最后补 `\0`
- 把“是否截断”作为正式返回信息

### 该函数更全局的命名建议

- `copy_cstr`
- `copy_bounded_string`
- `copy_string_truncating`

---

## 5.2 html_entity_decode_amp

当前签名：

```cpp
void html_entity_decode_amp(char *text);
```

### 它现在做了什么

它只做一件事：

- 把字符串中的 `&amp;` 原地替换成 `&`

实现方式是双指针：

- `read_p` 负责读原串
- `write_p` 负责写回结果
- 命中 `&amp;` 时写入一个 `&`
- 其余字符原样拷贝

### 它为什么存在

DLNA SOAP 请求中可能会把 URL 或元数据里的 `&` 编码成 `&amp;`，所以提取出标签内容后，代码需要把它还原。

### 它的问题

这个函数太具体了，只支持一种实体：

- 支持 `&amp;`
- 不支持 `&lt;`
- 不支持 `&gt;`
- 不支持 `&quot;`
- 不支持 `&apos;`
- 不支持十进制或十六进制数字实体

如果以后不是只处理 URL，而是要处理更一般的 XML/HTML 文本，这个函数很快就不够用。

### 更通用的抽象方式

你真正需要的能力一般是这两类之一：

#### 方案 A：通用实体解码

```cpp
size_t decode_html_entities_inplace(char *text);
```

适用于：

- 你确定输入是已经抽取出来的一段 XML/HTML 文本
- 你希望统一支持常见命名实体

#### 方案 B：通用替换器

```cpp
bool replace_substring_inplace(char *text, const char *from, const char *to);
```

适用于：

- 你只想做少量、可配置的替换
- 不想引入完整实体表

### 重写建议

如果你当前项目仍然只是 DLNA 设备控制，最现实的方案是：

- 先把它升级成 `decode_basic_xml_entities_inplace`
- 至少支持 `amp / lt / gt / quot / apos`

这样已经比现在稳很多，而且实现成本不高。

---

## 5.3 strip_angle_brackets

当前签名：

```cpp
void strip_angle_brackets(char *text);
```

### 它现在做了什么

它的流程是：

1. 先调用 `trim_ascii_whitespace`
2. 再检查字符串是否满足首字符是 `<` 且尾字符是 `>`
3. 如果满足，就把最外层这两个字符去掉

### 它的典型用途

当前主要用于处理 HTTP `CALLBACK` 头字段，例如：

```text
CALLBACK: <http://192.168.1.2:1234/event>
```

提取值并去掉尖括号后，才是实际 URL。

### 它的问题

- 名字仍然非常具体
- 它其实不是“处理尖括号”，而是“去掉外层包裹符”
- 只能处理 `<...>`
- 不能复用到 `"..."`、`'...'`、`[...]` 之类的通用包裹场景

### 更通用的抽象方式

建议直接抽象成：

```cpp
bool strip_surrounding_pair(char *text, char left, char right, bool trim_first = true);
```

或者：

```cpp
bool unwrap_if_wrapped(char *text, const char *prefix, const char *suffix, bool trim_first = true);
```

这样你可以统一处理：

- `<url>`
- `"token"`
- `'value'`
- `(expr)`

### 重写建议

把 `strip_angle_brackets` 变成一个兼容包装函数即可：

```cpp
inline bool strip_angle_brackets(char *text) {
    return strip_surrounding_pair(text, '<', '>');
}
```

这样底层能力通用，上层旧接口还能保留。

---

## 5.4 parse_http_url

当前签名：

```cpp
bool parse_http_url(const char *url, simple_http_url &out);
```

### 它现在做了什么

它目前只支持这种格式：

```text
http://host[:port][/path]
```

处理步骤是：

1. 检查输入不为空
2. 检查前缀是否是 `http://`
3. 定位第一个 `/`，把它当作路径起点
4. 在主机段里找最后一个 `:`，把它当作端口分隔符
5. 提取主机名
6. 如果有端口，就用 `atoi` 转成整数
7. 如果没有路径，默认路径为 `/`

### 它能满足当前业务的原因

因为当前拉流和回调 URL 都是很简单的 HTTP URL，所以这个实现足够跑通。

### 它的限制非常明显

#### 只支持 http

- 不支持 `https://`

#### 不是通用 URL 解析器

- 不支持 query 独立提取
- 不支持 fragment 独立提取
- 不支持 userinfo
- 不支持 IPv6 字面量
- 不支持协议省略

#### 端口解析不严格

因为用了 `atoi`，像下面这种输入有可能被错误接受：

```text
http://example.com:80abc/path
```

理论上这应该报错，但 `atoi` 会读到 `80` 就停。

#### 对输出结构过于固定

当前输出只有：

- host
- path
- port

这意味着如果你后面还要：

- 判断 scheme
- 区分 path 和 query
- 保存是否显式指定了端口

那就必须继续改结构体。

### 更通用的抽象方式

如果你的目标是“未来还能继续用”，建议改成真正的 URL 拆解结构：

```cpp
struct parsed_url {
    std::array<char, 16> scheme = {0};
    std::array<char, 128> host = {0};
    std::array<char, 16> port_text = {0};
    std::array<char, 512> path = {0};
    std::array<char, 256> query = {0};
    std::array<char, 128> fragment = {0};
    uint16_t port = 0;
    bool has_explicit_port = false;
};

enum class url_parse_result {
    ok,
    invalid_arg,
    unsupported_scheme,
    missing_host,
    invalid_port,
    host_too_long,
    path_too_long,
};

url_parse_result parse_url(const char *text, parsed_url &out);
```

### 如果你只想继续做轻量版

也可以保守一点，仍然只做 HTTP，但至少把接口写得更清楚：

```cpp
bool parse_http_endpoint(const char *text, parsed_url &out);
```

这里的重点是：

- 名字强调它不是全 URL 规范实现
- 返回结果尽量把错误原因分出来

### 重写时建议补的能力

- 严格端口解析，不接受脏字符
- 支持 query 保留在 path 中，或者单独拆出 query
- 明确是否允许没有 path
- 明确是否允许 host 是 IP、域名、localhost

---

## 5.5 resolve_ipv4_addr

当前签名：

```cpp
bool resolve_ipv4_addr(const char *host, in_addr *out_addr);
```

### 它现在做了什么

流程非常直接：

1. 检查参数
2. 先尝试把输入当作点分十进制 IPv4 字面量
3. 如果失败，再调用 `lwip_gethostbyname`
4. 检查返回记录是否为 IPv4
5. 拷贝第一个地址到输出

### 它的优点

- 先走字面量快路径，效率合理
- 对当前 lwIP 场景足够实用

### 它的问题

- 只支持 IPv4
- 只返回第一个结果
- 没有暴露错误原因
- DNS 解析失败、类型不匹配、参数错误都只返回 `false`
- 和 URL 解析层耦合较松，但命名还是偏结果导向，不利于以后扩展到 `sockaddr`

### 更通用的抽象方式

更适合的抽象通常是：

```cpp
enum class resolve_result {
    ok,
    invalid_arg,
    bad_literal,
    dns_failed,
    unsupported_family,
};

resolve_result resolve_host_ipv4(const char *host, in_addr *out_addr);
```

如果你后面可能做 TCP 连接封装，可以再上一层：

```cpp
bool resolve_endpoint_ipv4(const char *host, uint16_t port, sockaddr_in *out_addr);
```

### 更全局的建议

不要把“解析 URL”和“解析 DNS”混成一个函数。两者应该独立：

- URL 层只负责把文本拆成 host/port/path
- 解析层只负责把 host 变成地址

现在这点已经基本做对了，重写时保持这个分层即可。

---

## 5.6 extract_http_header_value

当前签名：

```cpp
bool extract_http_header_value(const char *request, const char *key, char *out, size_t out_size);
```

### 它现在做了什么

它会在完整 HTTP 报文文本中逐行扫描，寻找：

```text
Key: value
```

并且：

- key 比较时忽略大小写
- 找到后只跳过值前面的空格和制表符
- 把剩余内容复制到 `out`

### 它的真实定位

它不是“HTTP 解析器”，只是：

> 从完整报文里按字段名抓取某个头字段值。

### 当前实现的边界

#### 只适合简单头字段

- 默认按 `\r\n` 分行
- 不处理折行头字段
- 不处理重复头字段聚合
- 不处理值中的复杂参数拆分

#### 只去掉左侧空白

它不会自动去掉右侧空白，所以调用方经常还要再调一次 `trim_ascii_whitespace`。

#### 不支持“有没有这个头”与“提取值”分离

很多时候你只是想判断：

- 是否存在某个头
- 是否包含某个 token
- 是否匹配某个值

但当前接口一上来就要求你提供输出缓冲区。

### 更通用的抽象方式

建议拆成三层：

#### 第一层：找到 value 视图

```cpp
struct text_span {
    const char *ptr = nullptr;
    size_t len = 0;
};

bool find_http_header_value(const char *message, const char *key, text_span &out);
```

#### 第二层：需要复制时再复制

```cpp
bool copy_http_header_value(const char *message, const char *key, char *out, size_t out_size, bool trim = true);
```

#### 第三层：更直接的判断接口

```cpp
bool has_http_header(const char *message, const char *key);
bool http_header_value_equals(const char *message, const char *key, const char *expected, bool ignore_case = true);
bool http_header_value_contains(const char *message, const char *key, const char *token, bool ignore_case = true);
```

### 这就是你提到的“更全局”的关键点

你说“有些是检查是否有某个标签，能不能改为自定义检查有没有某个标签”，HTTP 头这里同样适用。

不要只保留“提取值”一个入口，应该至少有：

- 查找
- 复制
- 判断存在
- 判断相等
- 判断包含

这样调用方就不需要每次都自己拼逻辑。

### 重写建议

如果你仍然想保持轻量，不要一下子做完整 RFC 解析，最划算的是：

- 保持逐行扫描
- 增加 `text_span`
- 再用一层包装实现复制与判断

这样代码量不大，但复用度会明显提高。

---

## 5.7 trim_ascii_whitespace

当前签名：

```cpp
void trim_ascii_whitespace(char *text);
```

### 它现在做了什么

它会原地删除字符串首尾空白字符：

- 左侧连续空白跳过
- 右侧连续空白裁掉
- 必要时把中间正文搬移到头部

### 它的定位

这是所有文本处理代码里最值得保留的基础能力之一。

### 它的问题不大，但仍可继续抽象

当前版本已经够用，不过从“基础库设计”的角度，通常还可以补成：

```cpp
enum class trim_mode {
    left,
    right,
    both,
};

bool trim_ascii_inplace(char *text, trim_mode mode = trim_mode::both);
text_span trim_ascii_view(text_span input);
```

### 为什么建议加 view 版本

很多时候你只是想跳过两边空白，并不想修改原缓冲区。

例如：

- 扫描 HTTP 头时
- 扫描 XML 标签内容时
- 做状态字段比较时

如果有 `text_span` 版本，你可以：

- 先定位区域
- 再按需复制
- 不必反复 `memmove`

这对性能和代码结构都更好。

---

## 5.8 ascii_iequals

当前签名：

```cpp
bool ascii_iequals(const char *a, const char *b);
```

### 它现在做了什么

- 两个字符串逐字符比较
- 比较前都转成小写
- 如果长度不同或某个字符不同，返回 `false`

### 它的价值

这个函数在协议处理中非常常见，因为：

- HTTP 头字段名通常大小写不敏感
- 某些协议 token 也常做大小写无关比较

### 它的问题

主要问题不是逻辑错误，而是能力太单点：

- 只有“相等”
- 没有“前缀匹配”
- 没有“后缀匹配”
- 没有“局部区间比较”

### 更通用的抽象方式

建议至少形成一个小的 ASCII 比较族：

```cpp
char ascii_tolower_fast(char ch);
bool ascii_case_equal(const char *a, const char *b);
bool ascii_case_starts_with(const char *text, const char *prefix);
bool ascii_case_ends_with(const char *text, const char *suffix);
```

### 额外建议

如果你想彻底避免 `tolower` 的区域设置影响，可以自己实现只针对 `A-Z` 的 ASCII 小写转换。

这对嵌入式代码更可控。

---

## 5.9 ascii_icontains

当前签名：

```cpp
bool ascii_icontains(const char *haystack, const char *needle);
```

### 它现在做了什么

这是一个大小写无关的子串搜索：

- 外层枚举 haystack 每个起点
- 内层逐字符和 needle 比较
- 全匹配则返回 `true`

### 它的用途

当前它常被用来判断：

- 某头字段里是否含有某 token
- 元数据里是否出现某 MIME 类型关键词
- 状态描述里是否包含某关键字

### 它的隐藏设计问题

它把“搜索位置”和“是否包含”绑死了。

但更底层、更通用的能力应该是：

- 找到第一次命中的位置
- 如果只是想知道是否存在，再把结果转成 `bool`

### 更通用的抽象方式

建议底层接口写成：

```cpp
const char *ascii_case_find(const char *text, const char *pattern);
```

然后：

```cpp
inline bool ascii_case_contains(const char *text, const char *pattern) {
    return ascii_case_find(text, pattern) != nullptr;
}
```

### 为什么这样更好

因为你以后可能还需要：

- 找到 token 后继续向后解析参数
- 判断是不是完整单词边界
- 统计出现次数

如果底层直接返回位置，可复用性高很多。

### 额外注意

当前实现对空 `needle` 返回 `false`。这不是错，但你需要在新实现里明确约定。

常见选择有两种：

- 约定空模式非法，返回 `nullptr`
- 约定空模式总是匹配开头

只要文档写清楚即可。

---

## 5.10 extract_xml_tag_value

当前签名：

```cpp
char *extract_xml_tag_value(const char *buffer, const char *tag, char *out, size_t out_size);
```

### 它现在做了什么

它的逻辑是：

1. 拼出 `<tag>`
2. 拼出 `</tag>`
3. 用 `strstr` 找开始标签
4. 再找结束标签
5. 把中间文本复制到输出缓冲区
6. 成功时返回 `out`，失败时返回 `nullptr`

### 当前名字的问题最大

这个函数名字看起来像“XML 标签值提取器”，但实际上它只适用于最简单的纯文本包裹形式：

```xml
<CurrentURI>http://a/b.mp3</CurrentURI>
```

它不是通用 XML 解析器，以下情况都可能不正确：

- 标签带属性：`<tag attr="x">`
- 命名空间：`<u:Tag>`
- 空标签：`<tag></tag>`
- 自闭合标签：`<tag/>`
- 多个同名标签
- 标签嵌套
- CDATA
- 注释
- 实体混合处理

### 这里最值得做“全局化”重构

你刚才举的例子非常准确。

当前不是只有“提取标签值”一种需求。未来通常至少还会有：

- 检查有没有某个标签
- 找到某个标签的位置
- 提取某个标签的文本内容
- 提取多个同名标签
- 读取标签属性

所以建议最少拆成下面几类接口：

```cpp
bool has_simple_tag(const char *text, const char *tag);
bool find_simple_tag_span(const char *text, const char *tag, text_span &out_content);
bool copy_simple_tag_text(const char *text, const char *tag, char *out, size_t out_size, bool trim = false);
```

如果你还想进一步扩展，可以加：

```cpp
bool find_nth_simple_tag_span(const char *text, const char *tag, size_t index, text_span &out_content);
bool extract_tag_attribute(const char *text, const char *tag, const char *attr_name, char *out, size_t out_size);
```

### 重写时一定要先决定一件事

你到底要的是：

#### 方案 A：简单标签扫描器

特点：

- 轻量
- 不引入第三方 XML 库
- 适合已知格式的 SOAP 报文

#### 方案 B：真正的 XML 解析器

特点：

- 正确性更高
- 复杂度更高
- 在 MCU 上可能不划算

对于你当前项目，我建议先选方案 A，但名字必须改清楚，避免误导。

比如：

- `extract_simple_tag_text`
- `find_tag_content_span_loose`
- `scan_tag_content`

这样别人一看就知道它不是完整 XML 库。

### 当前实现的两个细节缺陷

#### 空内容会失败

当前要求：

```cpp
value_len > 0
```

所以：

```xml
<tag></tag>
```

会被当成失败。

#### 刚好装满时也会失败

当前条件是：

```cpp
value_len < out_size - 1
```

也就是说，如果内容长度刚好等于 `out_size - 1`，按理说完全能装下并留出结尾 `\0`，但它仍然返回失败。

这两个点在你重写时建议顺手修掉。

---

## 6. 从“具体函数”提升到“通用能力”的建议映射

下面这张表是这次重写最核心的部分。

| 现有函数 | 真实能力 | 更通用的新接口方向 |
| --- | --- | --- |
| copy_string_safe | 有界复制 C 字符串 | copy_cstr / append_cstr |
| html_entity_decode_amp | 原地替换特定实体 | decode_basic_xml_entities_inplace / replace_substring_inplace |
| strip_angle_brackets | 去掉最外层包裹符 | strip_surrounding_pair / unwrap_if_wrapped |
| parse_http_url | 解析简化版 HTTP 端点 | parse_url / parse_http_endpoint |
| resolve_ipv4_addr | host 到 IPv4 地址解析 | resolve_host_ipv4 / resolve_endpoint_ipv4 |
| extract_http_header_value | 查找并复制指定 HTTP 头值 | find_http_header_value / has_http_header / copy_http_header_value |
| trim_ascii_whitespace | 原地 ASCII trim | trim_ascii_inplace / trim_ascii_view |
| ascii_iequals | ASCII 大小写无关相等 | ascii_case_equal / ascii_case_starts_with |
| ascii_icontains | ASCII 大小写无关查找 | ascii_case_find / ascii_case_contains |
| extract_xml_tag_value | 简单标签内容扫描 | has_simple_tag / find_simple_tag_span / copy_simple_tag_text |

---

## 7. 你现在这个模块缺少哪些“很可能马上会用到”的能力

这是重写时非常值得补上的内容。

## 7.1 文本基础能力

- `ascii_case_find`
- `ascii_case_starts_with`
- `ascii_case_ends_with`
- `strip_surrounding_pair`
- `split_once`
- `parse_uint16_strict`

其中 `parse_uint16_strict` 很有用，因为 URL 端口、HTTP Content-Length、ICY metadata 长度这类字段都适合用严格数值解析，而不是 `atoi`。

## 7.2 HTTP 文本能力

- `find_http_header_value`
- `has_http_header`
- `copy_http_header_value`
- `split_http_head_and_body`
- `parse_http_request_line`
- `parse_http_status_line`
- `http_header_value_contains_token`

尤其是 `split_http_head_and_body` 很实用，因为你现在很多逻辑默认整个请求一次性收齐并且能直接 `strstr("\r\n\r\n")`。把这个动作单独抽出来，后续会清晰很多。

## 7.3 URL 能力

- `parse_url`
- `parse_host_port`
- `percent_decode_inplace`
- `build_host_header_value`

即使你暂时不做 query 参数解析，至少也该把“严格端口解析”和“scheme/host/path 拆分”做扎实。

## 7.4 标签扫描能力

- `has_simple_tag`
- `find_simple_tag_span`
- `copy_simple_tag_text`
- `find_nth_simple_tag_span`

如果你后面还会处理 SOAP 动作参数，这一层会反复用到。

## 7.5 文本解码能力

- `decode_basic_xml_entities_inplace`
- `decode_percent_encoding_inplace`

其中：

- `&amp;` 属于 XML/HTML 实体解码
- `%20` 属于 URL 百分号解码

这两类不要混在一个函数里。

---

## 8. 推荐的模块拆分方式

如果你要重写，我建议不要继续把所有函数放在一个 `http_utils.hpp` 里，可以拆成这样：

```text
http_utils/
  ascii_text.hpp
  ascii_text.cpp
  text_scan.hpp
  text_scan.cpp
  url_parse.hpp
  url_parse.cpp
  http_headers.hpp
  http_headers.cpp
  entity_decode.hpp
  entity_decode.cpp
  simple_tag_scan.hpp
  simple_tag_scan.cpp
  net_resolve.hpp
  net_resolve.cpp
```

如果你暂时不想拆这么细，至少也建议拆成三层：

- `text_utils`
- `protocol_scan_utils`
- `network_utils`

这样以后维护时不会再出现“明明是 XML/HTTP/DNS 混合工具，却全都挂在同一个头文件里”的情况。

---

## 9. 推荐的新接口草案

下面给你一个适合当前项目、又比现状更通用的轻量版接口草案。

```cpp
struct text_span {
    const char *ptr = nullptr;
    size_t len = 0;
};

enum class copy_result {
    ok,
    truncated,
    invalid_arg,
};

enum class url_parse_result {
    ok,
    invalid_arg,
    unsupported_scheme,
    missing_host,
    invalid_port,
    host_too_long,
    path_too_long,
};

copy_result copy_cstr(char *dst, size_t dst_size, const char *src, size_t *written = nullptr);
copy_result append_cstr(char *dst, size_t dst_size, const char *src, size_t *written = nullptr);

bool trim_ascii_inplace(char *text);
text_span trim_ascii_view(text_span span);

bool strip_surrounding_pair(char *text, char left, char right, bool trim_first = true);

bool ascii_case_equal(const char *a, const char *b);
const char *ascii_case_find(const char *text, const char *pattern);
bool ascii_case_contains(const char *text, const char *pattern);
bool ascii_case_starts_with(const char *text, const char *prefix);
bool ascii_case_ends_with(const char *text, const char *suffix);

url_parse_result parse_http_endpoint(const char *text, parsed_url &out);

bool find_http_header_value(const char *message, const char *key, text_span &out);
bool copy_http_header_value(const char *message, const char *key, char *out, size_t out_size, bool trim = true);
bool has_http_header(const char *message, const char *key);
bool http_header_value_contains(const char *message, const char *key, const char *token, bool ignore_case = true);

size_t decode_basic_xml_entities_inplace(char *text);

bool has_simple_tag(const char *text, const char *tag);
bool find_simple_tag_span(const char *text, const char *tag, text_span &out_content);
bool copy_simple_tag_text(const char *text, const char *tag, char *out, size_t out_size, bool trim = false);

bool resolve_host_ipv4(const char *host, in_addr *out_addr);
```

这套接口有几个好处：

- 仍然很轻量，适合 MCU
- 把“查找”和“复制”分开了
- 把“是否存在”和“提取文本”分开了
- 为后续扩展预留了空间

## 9.1 如果按这个方向重写，代码大概会长什么样

这一节不追求“完整可直接编译成你项目最终版本”，重点是给你一个足够接近最终落地形态的参考骨架。

你可以把它理解成：

- 接口怎么收敛
- 底层函数怎么组织
- 调用方最后会怎么写

### 9.1.1 一个更合理的头文件示例

```cpp
#ifndef HTTP_TEXT_UTILS_HPP
#define HTTP_TEXT_UTILS_HPP

#include <array>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
}

struct text_span {
    const char *ptr = nullptr;
    size_t len = 0;

    bool empty() const
    {
        return ptr == nullptr || len == 0;
    }
};

enum class copy_result {
    ok,
    truncated,
    invalid_arg,
};

enum class url_parse_result {
    ok,
    invalid_arg,
    unsupported_scheme,
    missing_host,
    invalid_port,
    host_too_long,
    path_too_long,
};

enum class resolve_result {
    ok,
    invalid_arg,
    dns_failed,
    unsupported_family,
};

struct parsed_url {
    std::array<char, 16> scheme = {0};
    std::array<char, 128> host = {0};
    std::array<char, 512> path = {0};
    std::array<char, 16> port_text = {0};
    uint16_t port = 80;
    bool has_explicit_port = false;
};

copy_result copy_cstr(char *dst, size_t dst_size, const char *src, size_t *written = nullptr);
bool trim_ascii_inplace(char *text);
bool strip_surrounding_pair(char *text, char left, char right, bool trim_first = true);

char ascii_tolower_fast(char ch);
bool ascii_case_equal(const char *a, const char *b);
const char *ascii_case_find(const char *text, const char *pattern);
bool ascii_case_contains(const char *text, const char *pattern);

text_span trim_ascii_view(text_span span);
bool find_http_header_value(const char *message, const char *key, text_span &out);
bool copy_http_header_value(const char *message, const char *key, char *out, size_t out_size, bool trim = true);
bool has_http_header(const char *message, const char *key);

bool find_simple_tag_span(const char *text, const char *tag, text_span &out_content);
bool copy_simple_tag_text(const char *text, const char *tag, char *out, size_t out_size, bool trim = false);
size_t decode_basic_xml_entities_inplace(char *text);

url_parse_result parse_http_endpoint(const char *text, parsed_url &out);
resolve_result resolve_host_ipv4(const char *host, in_addr *out_addr);

#endif
```

这个版本和你当前的区别是：

- 明确引入了 `text_span`
- 复制、查找、判断分成不同层次
- URL 和 DNS 都有更清晰的返回结果
- “简单标签扫描器”被明确标注为 simple，而不是伪装成完整 XML 解析器

### 9.1.2 copy_cstr 的示例实现

```cpp
copy_result copy_cstr(char *dst, size_t dst_size, const char *src, size_t *written)
{
    if (dst == nullptr || dst_size == 0) {
        return copy_result::invalid_arg;
    }

    if (src == nullptr) {
        dst[0] = '\0';
        if (written != nullptr) {
            *written = 0;
        }
        return copy_result::ok;
    }

    size_t index = 0;
    while (src[index] != '\0' && index + 1 < dst_size) {
        dst[index] = src[index];
        ++index;
    }
    dst[index] = '\0';

    if (written != nullptr) {
        *written = index;
    }

    if (src[index] != '\0') {
        return copy_result::truncated;
    }
    return copy_result::ok;
}
```

这个版本和旧实现相比，最大的提升是：

- 可以知道是否截断
- 可以拿到写入长度
- 不依赖 `snprintf`

调用示例：

```cpp
std::array<char, 16> name = {0};
size_t written = 0;
copy_result result = copy_cstr(name.data(), name.size(), "renderer-device-name", &written);
if (result == copy_result::truncated) {
    osal_printk("设备名被截断, written=%u\n", static_cast<unsigned>(written));
}
```

### 9.1.3 trim_ascii_inplace 和 strip_surrounding_pair 的示例实现

```cpp
bool trim_ascii_inplace(char *text)
{
    if (text == nullptr) {
        return false;
    }

    size_t len = 0;
    while (text[len] != '\0') {
        ++len;
    }

    size_t start = 0;
    while (start < len) {
        char ch = text[start];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\f' && ch != '\v') {
            break;
        }
        ++start;
    }

    size_t end = len;
    while (end > start) {
        char ch = text[end - 1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\f' && ch != '\v') {
            break;
        }
        --end;
    }

    if (start > 0) {
        memmove(text, text + start, end - start);
    }
    text[end - start] = '\0';
    return true;
}

bool strip_surrounding_pair(char *text, char left, char right, bool trim_first)
{
    if (text == nullptr) {
        return false;
    }

    if (trim_first) {
        trim_ascii_inplace(text);
    }

    size_t len = strlen(text);
    if (len < 2) {
        return false;
    }
    if (text[0] != left || text[len - 1] != right) {
        return false;
    }

    memmove(text, text + 1, len - 2);
    text[len - 2] = '\0';
    return true;
}
```

调用示例：

```cpp
std::array<char, 128> callback = {0};
copy_cstr(callback.data(), callback.size(), "  <http://192.168.1.2:1400/event>  ");
strip_surrounding_pair(callback.data(), '<', '>', true);
// 结果: http://192.168.1.2:1400/event
```

这就是为什么我建议把原来的 `strip_angle_brackets` 上提为更通用的 `strip_surrounding_pair`。

### 9.1.4 ASCII 大小写无关比较/查找示例

```cpp
char ascii_tolower_fast(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

bool ascii_case_equal(const char *a, const char *b)
{
    if (a == nullptr || b == nullptr) {
        return false;
    }

    while (*a != '\0' && *b != '\0') {
        if (ascii_tolower_fast(*a) != ascii_tolower_fast(*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return (*a == '\0' && *b == '\0');
}

const char *ascii_case_find(const char *text, const char *pattern)
{
    if (text == nullptr || pattern == nullptr || pattern[0] == '\0') {
        return nullptr;
    }

    for (size_t i = 0; text[i] != '\0'; ++i) {
        size_t j = 0;
        while (pattern[j] != '\0' && text[i + j] != '\0' &&
               ascii_tolower_fast(text[i + j]) == ascii_tolower_fast(pattern[j])) {
            ++j;
        }
        if (pattern[j] == '\0') {
            return text + i;
        }
    }
    return nullptr;
}

bool ascii_case_contains(const char *text, const char *pattern)
{
    return ascii_case_find(text, pattern) != nullptr;
}
```

调用示例：

```cpp
if (ascii_case_contains("Transfer-Encoding: Chunked", "chunked")) {
    osal_printk("命中 chunked 传输\n");
}

if (ascii_case_equal("PLAYING", "playing")) {
    osal_printk("状态匹配\n");
}
```

### 9.1.5 text_span + HTTP 头字段扫描示例

这里是最值得你这次重构时直接照着落地的一段，因为它能把“查找”和“复制”彻底分开。

```cpp
text_span trim_ascii_view(text_span span)
{
    if (span.ptr == nullptr) {
        return {};
    }

    size_t start = 0;
    while (start < span.len) {
        char ch = span.ptr[start];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\f' && ch != '\v') {
            break;
        }
        ++start;
    }

    size_t end = span.len;
    while (end > start) {
        char ch = span.ptr[end - 1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\f' && ch != '\v') {
            break;
        }
        --end;
    }

    return {span.ptr + start, end - start};
}

static bool ascii_case_equal_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (a[i] == '\0' || b[i] == '\0') {
            return false;
        }
        if (ascii_tolower_fast(a[i]) != ascii_tolower_fast(b[i])) {
            return false;
        }
    }
    return true;
}

bool find_http_header_value(const char *message, const char *key, text_span &out)
{
    out = {};
    if (message == nullptr || key == nullptr || key[0] == '\0') {
        return false;
    }

    const size_t key_len = strlen(key);
    const char *line = message;

    while (*line != '\0') {
        const char *line_end = strstr(line, "\r\n");
        if (line_end == nullptr) {
            line_end = line + strlen(line);
        }

        const size_t line_len = static_cast<size_t>(line_end - line);
        if (line_len > key_len && ascii_case_equal_n(line, key, key_len) && line[key_len] == ':') {
            text_span raw = {line + key_len + 1, line_len - key_len - 1};
            out = trim_ascii_view(raw);
            return true;
        }

        if (*line_end == '\0') {
            break;
        }
        line = line_end + 2;
    }

    return false;
}

bool copy_http_header_value(const char *message, const char *key, char *out, size_t out_size, bool trim)
{
    if (out == nullptr || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    text_span span;
    if (!find_http_header_value(message, key, span)) {
        return false;
    }
    if (!trim) {
        const char *line = span.ptr;
        size_t len = span.len;
        copy_result result = copy_cstr(out, out_size, "", nullptr);
        (void)result;
        size_t copy_len = (len < out_size - 1) ? len : (out_size - 1);
        memcpy(out, line, copy_len);
        out[copy_len] = '\0';
        return true;
    }

    size_t copy_len = (span.len < out_size - 1) ? span.len : (out_size - 1);
    memcpy(out, span.ptr, copy_len);
    out[copy_len] = '\0';
    return true;
}

bool has_http_header(const char *message, const char *key)
{
    text_span span;
    return find_http_header_value(message, key, span);
}
```

调用示例一，只判断有没有：

```cpp
if (has_http_header(request, "SID")) {
    osal_printk("收到订阅 SID\n");
}
```

调用示例二，提取值：

```cpp
std::array<char, 128> content_type = {0};
if (copy_http_header_value(response, "Content-Type", content_type.data(), content_type.size(), true)) {
    osal_printk("Content-Type=%s\n", content_type.data());
}
```

调用示例三，不复制，直接拿视图：

```cpp
text_span transfer_encoding;
if (find_http_header_value(response, "Transfer-Encoding", transfer_encoding)) {
    if (ascii_case_find(transfer_encoding.ptr, "chunked") != nullptr) {
        osal_printk("检测到 chunked\n");
    }
}
```

这部分就是你前面提到的“能不能更全局一点”的直接落地版本。

### 9.1.6 简单标签扫描器示例

这个版本明确告诉使用者：它只是一个轻量扫描器，只适合已知格式文本，不是完整 XML 解析器。

```cpp
bool find_simple_tag_span(const char *text, const char *tag, text_span &out_content)
{
    out_content = {};
    if (text == nullptr || tag == nullptr || tag[0] == '\0') {
        return false;
    }

    char start_tag[64] = {0};
    char end_tag[64] = {0};
    snprintf(start_tag, sizeof(start_tag), "<%s>", tag);
    snprintf(end_tag, sizeof(end_tag), "</%s>", tag);

    const char *start = strstr(text, start_tag);
    if (start == nullptr) {
        return false;
    }
    start += strlen(start_tag);

    const char *end = strstr(start, end_tag);
    if (end == nullptr) {
        return false;
    }

    out_content.ptr = start;
    out_content.len = static_cast<size_t>(end - start);
    return true;
}

bool copy_simple_tag_text(const char *text, const char *tag, char *out, size_t out_size, bool trim)
{
    if (out == nullptr || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    text_span span;
    if (!find_simple_tag_span(text, tag, span)) {
        return false;
    }

    text_span final_span = trim ? trim_ascii_view(span) : span;
    size_t copy_len = (final_span.len < out_size - 1) ? final_span.len : (out_size - 1);
    memcpy(out, final_span.ptr, copy_len);
    out[copy_len] = '\0';
    return true;
}
```

调用示例：

```cpp
std::array<char, 512> media_url = {0};
std::array<char, 1024> media_metadata = {0};

if (copy_simple_tag_text(buffer.data(), "CurrentURI", media_url.data(), media_url.size(), true)) {
    decode_basic_xml_entities_inplace(media_url.data());
}

if (copy_simple_tag_text(buffer.data(), "CurrentURIMetaData", media_metadata.data(), media_metadata.size(), false)) {
    decode_basic_xml_entities_inplace(media_metadata.data());
}
```

如果你只想判断标签是否存在，不需要复制：

```cpp
text_span uri_span;
if (find_simple_tag_span(buffer.data(), "CurrentURI", uri_span)) {
    osal_printk("SOAP 里带 CurrentURI 标签\n");
}
```

### 9.1.7 decode_basic_xml_entities_inplace 的示例实现

```cpp
static bool match_entity(const char *text, const char *entity)
{
    while (*entity != '\0') {
        if (*text == '\0' || *text != *entity) {
            return false;
        }
        ++text;
        ++entity;
    }
    return true;
}

size_t decode_basic_xml_entities_inplace(char *text)
{
    if (text == nullptr) {
        return 0;
    }

    char *read_p = text;
    char *write_p = text;
    size_t replaced = 0;

    while (*read_p != '\0') {
        if (match_entity(read_p, "&amp;")) {
            *write_p++ = '&';
            read_p += 5;
            ++replaced;
        } else if (match_entity(read_p, "&lt;")) {
            *write_p++ = '<';
            read_p += 4;
            ++replaced;
        } else if (match_entity(read_p, "&gt;")) {
            *write_p++ = '>';
            read_p += 4;
            ++replaced;
        } else if (match_entity(read_p, "&quot;")) {
            *write_p++ = '"';
            read_p += 6;
            ++replaced;
        } else if (match_entity(read_p, "&apos;")) {
            *write_p++ = '\'';
            read_p += 6;
            ++replaced;
        } else {
            *write_p++ = *read_p++;
        }
    }

    *write_p = '\0';
    return replaced;
}
```

调用示例：

```cpp
std::array<char, 256> text = {0};
copy_cstr(text.data(), text.size(), "Tom &amp; Jerry &lt;theme&gt;", nullptr);
decode_basic_xml_entities_inplace(text.data());
// 结果: Tom & Jerry <theme>
```

### 9.1.8 parse_http_endpoint 的示例实现

这个实现依然是轻量版，但已经比当前版本更接近“可维护的正式接口”。

```cpp
static bool parse_uint16_strict(const char *text, uint16_t &value)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    uint32_t result = 0;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        char ch = text[i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        result = result * 10 + static_cast<uint32_t>(ch - '0');
        if (result > 65535U) {
            return false;
        }
    }

    value = static_cast<uint16_t>(result);
    return true;
}

/// @brief 解析简化版 HTTP 端点 (scheme://host[:port][/path])
/// 
/// 本函数实现轻量级 HTTP URL 解析，适用于已知格式的嵌入式场景
/// （如 DLNA 设备控制、HTTP 拉流）。
/// 
/// 支持的格式示例:
///   - http://example.com
///   - http://example.com:8080
///   - http://example.com/path/to/resource
///   - http://192.168.1.1:1234/stream.mp3
///
/// 不支持的特性（超出范围）：
///   - HTTPS 或其他 scheme
///   - IPv6 字面量 [::1]
///   - 用户信息 (userinfo@host)
///   - Query 字符串独立拆分 (query 保留在 path 中)
///   - Fragment (#anchor)
///
/// @param text 待解析的 URL 字符串，必须以 \0 结尾
/// @param out 输出的解析结果结构体，函数入口会清零，失败时内容不可靠
///
/// @return 解析结果枚举值：
///   - ok: 解析成功，out 中的所有字段都有效
///   - invalid_arg: text == nullptr，参数无效
///   - unsupported_scheme: 不以 "http://" 开头
///   - missing_host: host 部分为空 (如 "http://")
///   - host_too_long: host 长度超过输出缓冲区大小
///   - invalid_port: 端口号格式不合法或超过 65535
///   - path_too_long: path 长度超过输出缓冲区大小
///
/// @note 
///   - 此函数为轻量实现，不是完整 RFC 3986 解析器
///   - 端口号采用严格解析，拒绝非纯数字或溢出的输入
///   - 未显式指定端口时默认为 80，has_explicit_port == false
///   - path 包含 query（未单独拆分），如需解析应由调用者处理
url_parse_result parse_http_endpoint(const char *text, parsed_url &out)
{
    // 清零输出结构，确保即使失败返回也不会有脏数据
    out = {};
    if (text == nullptr) {
        return url_parse_result::invalid_arg;
    }

    // 检查 scheme 是否为 "http://"（固定前缀，长度 7）
    static const char *k_scheme = "http://";
    if (strncmp(text, k_scheme, 7) != 0) {
        return url_parse_result::unsupported_scheme;
    }

    // 记录 scheme 到输出结构
    copy_cstr(out.scheme.data(), out.scheme.size(), "http", nullptr);
    
    // p 指向 scheme 后面的部分（即 host[:port][/path]）
    const char *p = text + 7;
    
    // 寻找路径起点（第一个 /）
    const char *path_start = strchr(p, '/');
    
    // host_end 指向 host 部分的结束位置
    // 如果找到了路径，host 在路径开始前；否则 host 在字符串末尾
    const char *host_end = (path_start != nullptr) ? path_start : (p + strlen(p));

    // 在 host 部分扫描端口分隔符（:），记录最后一次出现的位置
    // （支持 IPv6 需要更复杂的逻辑，这里仅限 IPv4 和 domain name）
    const char *port_sep = nullptr;
    for (const char *it = p; it < host_end; ++it) {
        if (*it == ':') {
            port_sep = it;
        }
    }

    // 计算 host 的实际长度
    // 如果找到 :，host 在 : 之前；否则 host 就是 p 到 host_end 的全部
    size_t host_len = (port_sep != nullptr) ? static_cast<size_t>(port_sep - p) : static_cast<size_t>(host_end - p);
    
    // 检查 host 是否为空
    if (host_len == 0) {
        return url_parse_result::missing_host;
    }
    
    // 检查 host 是否超过缓冲区容量
    if (host_len >= out.host.size()) {
        return url_parse_result::host_too_long;
    }

    // 复制 host 到输出结构并补上 \0
    memcpy(out.host.data(), p, host_len);
    out.host[host_len] = '\0';

    // 处理端口号部分
    if (port_sep != nullptr) {
        // 端口号存在，需要提取并严格验证
        char port_buf[16] = {0};
        
        // 计算端口号的长度（从 : 后面到 host 结尾）
        size_t port_len = static_cast<size_t>(host_end - port_sep - 1);
        
        // 检查端口号是否为空或超过缓冲区
        if (port_len == 0 || port_len >= sizeof(port_buf)) {
            return url_parse_result::invalid_port;
        }
        
        // 复制端口号字符串到临时缓冲区
        memcpy(port_buf, port_sep + 1, port_len);
        port_buf[port_len] = '\0';

        // 严格解析端口号（拒绝非数字或超过 65535 的值）
        uint16_t port = 0;
        if (!parse_uint16_strict(port_buf, port)) {
            return url_parse_result::invalid_port;
        }
        
        // 记录解析到的端口号及元数据
        out.port = port;
        out.has_explicit_port = true;
        copy_cstr(out.port_text.data(), out.port_text.size(), port_buf, nullptr);
    } else {
        // 未指定端口，使用 HTTP 默认端口 80
        out.port = 80;
        out.has_explicit_port = false;
        out.port_text[0] = '\0';
    }

    // 处理路径部分
    // 如果有路径，使用找到的 path_start；否则使用默认路径 "/"
    const char *path = (path_start != nullptr) ? path_start : "/";
    
    // 检查路径是否超过缓冲区容量
    if (strlen(path) >= out.path.size()) {
        return url_parse_result::path_too_long;
    }
    
    // 复制路径到输出结构
    copy_cstr(out.path.data(), out.path.size(), path, nullptr);
    
    // 解析成功
    return url_parse_result::ok;
}
```

调用示例：

```cpp
parsed_url url;
url_parse_result result = parse_http_endpoint("http://example.com:8080/live/stream.mp3", url);
if (result == url_parse_result::ok) {
    osal_printk("scheme=%s host=%s port=%u path=%s\n",
                url.scheme.data(),
                url.host.data(),
                static_cast<unsigned>(url.port),
                url.path.data());
}
```

### 9.1.9 resolve_host_ipv4 的示例实现

```cpp
/// @brief 将主机名或 IPv4 字面量解析为 IPv4 地址
/// 
/// 这是一个双路径的 IPv4 地址解析函数，优先尝试将输入视为点分十进制 IPv4 字面量，
/// 如果失败则通过 DNS 解析（使用 lwIP gethostbyname）。
///
/// @details
/// 工作流程：
/// 1. 参数合法性检查（host 和 out_addr 都必须非空）
/// 2. 快路径：尝试用 inet_aton() 解析输入为点分十进制 IPv4 地址
///    - 例如 "192.168.1.1"、"127.0.0.1" 等直接返回
///    - 这条路径避免了不必要的 DNS 查询，性能较好
/// 3. 慢路径：调用 lwip_gethostbyname() 进行 DNS 查询
///    - 输入被视为域名（如 "example.com"、"device.local"）
///    - DNS 查询可能涉及网络往返，耗时较长
/// 4. 验证 DNS 查询结果
///    - 检查返回的 hostent 结构体及其地址列表是否有效
///    - 验证返回的地址族是否为 AF_INET（IPv4）
///    - 验证返回的地址长度是否足以容纳 IPv4 地址（通常 4 字节）
/// 5. 拷贝第一个 IPv4 地址到输出缓冲区
///
/// @param host 待解析的主机标识符，支持两种格式：
///   - IPv4 点分十进制字面量，例如 "192.168.1.1"
///   - 域名或主机名，例如 "192-168-1-2.example.com"、"router.local"
///   - 必须以 \0 结尾
///   - 不能为 nullptr
///
/// @param out_addr 输出 IPv4 地址的缓冲区指针
///   - 成功时，存放解析到的 IPv4 地址（网络字节序）
///   - 可以直接用于 struct sockaddr_in 的 sin_addr 字段
///   - 必须指向有效的 struct in_addr 空间（4 字节）
///   - 调用前不需要初始化，失败时内容不可靠
///   - 不能为 nullptr
///
/// @return resolve_result 枚举值，表示解析结果和失败原因：
///   - ok：解析成功，out_addr 中存放有效的 IPv4 地址
///   - invalid_arg：参数检查失败
///     * host == nullptr，或
///     * out_addr == nullptr
///   - dns_failed：DNS 查询失败或无有效结果
///     * lwip_gethostbyname() 返回 nullptr，或
///     * 返回的 hostent 的 h_addr_list 为空，或
///     * 地址列表的第一项为空指针
///   - unsupported_family：返回的地址不是 IPv4
///     * h_addrtype != AF_INET（不是 IPv4 地址族），或
///     * h_length 太短，无法容纳完整的 IPv4 地址
///
/// @note
///   - inet_aton() 调用成功（返回非 0）即直接返回，不再进行 DNS 查询
///   - DNS 查询使用 lwIP 协议栈，需要联网络模块支持
///   - 此函数只支持 IPv4，不支持 IPv6
///   - DNS 查询的超时和重试行为由 lwIP 配置决定
///   - 返回的 in_addr 数据已是网络字节序，可直接用于 socket 操作
///
/// @warning
///   - 如果 out_addr 指向的缓冲区小于 sizeof(in_addr)，调用后行为未定义
///   - DNS 查询可能阻塞，调用前应确认在可接受的延时范围内
///   - 某些嵌入式环境中 DNS 解析可能返回错误，应充分测试
///
/// @example
/// @code
/// // 示例 1：解析 IPv4 字面量（快路径）
/// in_addr addr = {};
/// resolve_result result = resolve_host_ipv4("192.168.1.1", &addr);
/// if (result == resolve_result::ok) {
///     printf("地址解析成功: 0x%08x\n", addr.s_addr);
/// }
///
/// // 示例 2：解析域名（慢路径，需要 DNS）
/// in_addr addr = {};
/// resolve_result result = resolve_host_ipv4("example.com", &addr);
/// switch (result) {
///     case resolve_result::ok:
///         printf("域名解析成功\n");
///         break;
///     case resolve_result::dns_failed:
///         printf("DNS 查询失败，可能是网络不可达或域名不存在\n");
///         break;
///     case resolve_result::unsupported_family:
///         printf("DNS 返回的不是 IPv4 地址\n");
///         break;
///     default:
///         printf("参数错误\n");
/// }
///
/// // 示例 3：用于建立 TCP 连接
/// in_addr remote_addr = {};
/// if (resolve_host_ipv4("mqtt.example.com", &remote_addr) == resolve_result::ok) {
///     struct sockaddr_in server_addr = {};
///     server_addr.sin_family = AF_INET;
///     server_addr.sin_addr = remote_addr;
///     server_addr.sin_port = htons(1883);
///     connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
/// }
/// @endcode
///
resolve_result resolve_host_ipv4(const char *host, in_addr *out_addr)
{
    if (host == nullptr || out_addr == nullptr) {
        return resolve_result::invalid_arg;
    }

    if (inet_aton(host, out_addr) != 0) {
        return resolve_result::ok;
    }

    hostent *entry = lwip_gethostbyname(host);
    if (entry == nullptr || entry->h_addr_list == nullptr || entry->h_addr_list[0] == nullptr) {
        return resolve_result::dns_failed;
    }
    if (entry->h_addrtype != AF_INET || entry->h_length < static_cast<int>(sizeof(in_addr))) {
        return resolve_result::unsupported_family;
    }

    memcpy(out_addr, entry->h_addr_list[0], sizeof(in_addr));
    return resolve_result::ok;
}
```

调用示例：

```cpp
in_addr addr = {};
resolve_result result = resolve_host_ipv4(url.host.data(), &addr);
if (result != resolve_result::ok) {
    osal_printk("主机解析失败: %s\n", url.host.data());
}
```

### 9.1.10 兼容旧接口时可以怎么包一层

这一步很重要，因为它能让你先把底层能力换掉，再慢慢迁移上层调用。

```cpp
void copy_string_safe(char *dst, size_t dst_size, const char *src)
{
    (void)copy_cstr(dst, dst_size, src, nullptr);
}

void strip_angle_brackets(char *text)
{
    (void)strip_surrounding_pair(text, '<', '>', true);
}

bool ascii_iequals(const char *a, const char *b)
{
    return ascii_case_equal(a, b);
}

bool ascii_icontains(const char *haystack, const char *needle)
{
    return ascii_case_contains(haystack, needle);
}

bool extract_http_header_value(const char *request, const char *key, char *out, size_t out_size)
{
    return copy_http_header_value(request, key, out, out_size, false);
}

char *extract_xml_tag_value(const char *buffer, const char *tag, char *out, size_t out_size)
{
    if (!copy_simple_tag_text(buffer, tag, out, out_size, false)) {
        return nullptr;
    }
    return out;
}
```

这个过渡层的意义是：

- 老代码先不动
- 新能力先落地
- 后面逐步把调用点改到新接口

### 9.1.11 调用方改完以后会是什么样

以你现在的 SUBSCRIBE 处理逻辑为例，旧写法是“提取值后再 trim，再做尖括号剥离”。

更推荐的写法会像这样：

```cpp
std::array<char, 256> sid_value = {0};
std::array<char, 256> callback_value = {0};

const bool has_sid = copy_http_header_value(buffer.data(), "SID", sid_value.data(), sid_value.size(), true);
const bool has_callback =
    copy_http_header_value(buffer.data(), "CALLBACK", callback_value.data(), callback_value.size(), true);

if (has_callback) {
    strip_surrounding_pair(callback_value.data(), '<', '>', false);
}

if (has_sid && sid_value[0] != '\0') {
    copy_cstr(g_avt_sid.data(), g_avt_sid.size(), sid_value.data(), nullptr);
}
if (has_callback && callback_value[0] != '\0') {
    copy_cstr(g_avt_callback.data(), g_avt_callback.size(), callback_value.data(), nullptr);
}
```

再比如 SOAP 标签提取，旧写法是：

```cpp
extract_xml_tag_value(buffer.data(), "CurrentURI", media_url.data(), media_url.size());
extract_xml_tag_value(buffer.data(), "CurrentURIMetaData", media_metadata.data(), media_metadata.size());
html_entity_decode_amp(media_url.data());
html_entity_decode_amp(media_metadata.data());
```

新写法可以变成：

```cpp
copy_simple_tag_text(buffer.data(), "CurrentURI", media_url.data(), media_url.size(), true);
copy_simple_tag_text(buffer.data(), "CurrentURIMetaData", media_metadata.data(), media_metadata.size(), false);
decode_basic_xml_entities_inplace(media_url.data());
decode_basic_xml_entities_inplace(media_metadata.data());
```

这时语义会清楚很多：

- `copy_simple_tag_text` 明确表示这是简单标签扫描
- `decode_basic_xml_entities_inplace` 明确表示这是 XML 实体解码
- 调用者一眼就能看出每一步在干什么

---

## 10. 推荐的重写顺序

不要一口气全重写，建议按依赖从低到高来。

## 第一步：先重写最底层文本工具

优先完成：

- copy_cstr
- trim_ascii_inplace
- ascii_case_equal
- ascii_case_find
- strip_surrounding_pair

原因：

- 这些函数依赖最少
- 出问题最容易单测
- 后面 HTTP、XML、URL 都会用到它们

## 第二步：补 `text_span` 思维

接着实现：

- text_span
- trim_ascii_view
- split_once
- 基于 span 的扫描函数

原因：

- 这样能避免过早复制字符串
- 后面做头字段、标签扫描会明显更顺手

## 第三步：重写 HTTP 头扫描

优先做：

- find_http_header_value
- has_http_header
- copy_http_header_value

## 第四步：重写简单标签扫描

优先做：

- has_simple_tag
- find_simple_tag_span
- copy_simple_tag_text

## 第五步：重写 URL 解析和网络解析

最后做：

- parse_http_endpoint
- resolve_host_ipv4

原因：

- 它们和外部输入格式、网络环境关系更强
- 在底层字符串工具稳定后，写起来更稳

---

## 11. 测试清单

你重写时强烈建议按下面这份单测清单来。

## 11.1 copy_cstr

- `src == nullptr`
- `dst == nullptr`
- `dst_size == 0`
- 完整复制
- 恰好装满
- 发生截断

## 11.2 trim_ascii_inplace

- 空串
- 全空白
- 左右都有空白
- 只有左边空白
- 只有右边空白
- 中间空白保留

## 11.3 strip_surrounding_pair

- `<abc>`
- ` <abc> `
- `<abc`
- `abc>`
- `"abc"`
- 空内容包裹 `<>`

## 11.4 ascii_case_find / equal

- 完全相同
- 大小写不同
- 一长一短
- 空字符串
- 空 pattern

## 11.5 find_http_header_value

- 标准头字段
- 大小写不同的 key
- 头值前有空格
- 头值后有空格
- 报文中没有该头
- 出现重复头字段
- 只有 `\n` 没有 `\r\n` 的异常输入

## 11.6 parse_http_endpoint

- `http://example.com`
- `http://example.com/`
- `http://example.com:8080/path`
- `http://127.0.0.1:80/test`
- 缺 host
- 非法 port
- `https://` 输入
- 超长 host
- 超长 path

## 11.7 simple tag scan

- `<tag>value</tag>`
- `<tag></tag>`
- 缺开始标签
- 缺结束标签
- 多个同名标签
- 标签带属性
- 命名空间标签

## 11.8 entity decode

- `a&amp;b`
- 连续多个实体
- 未闭合实体
- `&lt;` `&gt;` `&quot;` `&apos;`

---

## 12. 如果你想兼容旧代码，最稳的做法

如果你担心一次性改调用点风险太高，推荐做法不是“直接替换全部名字”，而是：

### 先实现新的通用底层接口

例如：

- `copy_cstr`
- `find_http_header_value`
- `copy_simple_tag_text`

### 再用旧名字做薄包装

例如：

```cpp
inline void copy_string_safe(char *dst, size_t dst_size, const char *src) {
    (void)copy_cstr(dst, dst_size, src, nullptr);
}

inline bool extract_http_header_value(const char *request, const char *key, char *out, size_t out_size) {
    return copy_http_header_value(request, key, out, out_size, false);
}
```

这样可以实现：

- 新代码开始使用更通用接口
- 老代码继续能跑
- 重构风险分阶段控制

---

## 13. 最后给你的结论

这组函数不是不能用，而是“抽象层级不整齐”。

最值得你这次重写时抓住的主线不是逐个修补，而是下面三件事：

### 13.1 把“查找”和“复制”拆开

这是当前代码里最重要的结构提升点。

适用于：

- HTTP 头字段
- XML 标签内容
- 文本 token 搜索

### 13.2 把“具体场景函数”提升成“通用原语”

典型例子：

- `strip_angle_brackets` 升级成 `strip_surrounding_pair`
- `html_entity_decode_amp` 升级成 `decode_basic_xml_entities_inplace`
- `extract_xml_tag_value` 升级成 `has_tag + find_tag_span + copy_tag_text`

### 13.3 用更明确的返回值表达错误和截断

尤其是：

- 复制是否截断
- URL 为什么解析失败
- DNS 为什么失败

这样你后续调试网络场景时，效率会高很多。

如果只用一句话总结这次重写方向，就是：

> 不要再围绕“当前业务名字”去加函数，而要围绕“文本扫描原语、协议扫描原语、网络原语”去设计接口。
