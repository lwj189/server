// ===========================================================================
// C++ 靶场 [1]：请求行解析
//
// 靶场 = 一个有明确判据的小练习：打完立刻知道中没中。
//   靶子：把一行 "GET /a?b=1 HTTP/1.1" 拆成 method / target / version
//   靶纸：tests/parse_test.py
//
// 打靶流程（一条命令，红还是绿）：
//     python3 tests/parse_test.py
//
// 编译（测试脚本会自己编译，你手动编也可以）：
//     g++ -std=c++17 -Wall -Wextra -Wformat=2 -Wconversion request_line.cpp -o request_line
// ===========================================================================

#include <iostream>   // std::cin / std::cout / std::getline
#include <string>     // std::string

// 请求行拆出来的三块
struct RequestLine {
    std::string method;    // "GET"
    std::string target;    // "/a?b=1"
    std::string version;   // "HTTP/1.1"
    bool ok = false;       // 拆成功了没（false = 这行不合法）
};

// ===========================================================================
// ★ 靶子：把一行请求行拆成 method / target / version
//
// 【契约定死了，照着做就行】—— 下面 5 条是【定】的，不是【查】的：
//   1. 恰好 3 段；连续多个空白算【一个】分隔符（RFC 9112 §3 允许这种宽容解析）
//   2. 首尾空白忽略
//   3. 三块都必须非空
//   4. target 原样保留 —— `?query` 算它的一部分（不做拆分）
//   5. 不是"恰好 3 段" → ok 保持 false，三块留空
//
// 【先想清楚：一个字符串怎么"切"成三段？】
//   第一段 = 从开头到第一个空白
//   第二段 = 第一个空白之后，到下一个空白
//   第三段 = 剩下的全部
//   所以整件事只有两个动作：【找位置】和【取子串】。
//
// 【该查的：std::string 有哪些成员函数可以用】
//   一条命令列出全部成员函数名（就是英文单词，不用背）：
//     grep -oP "^      \K[a-z_]+(?=\()" /usr/include/c++/15/bits/basic_string.h | sort -u | tr '\n' ' '
//   ★ 你要找的是这几类：找位置 / 取子串 / 判断"空白"
//
// 【⚠ 一个坑，先提醒】空白 ≠ 只有空格。TCP 那头可能发来 '\t'。
//   而"跳过空白"和"跳过某些字符"在标准库里是两种不同的写法，查清楚再选。
// ===========================================================================
RequestLine parseRequestLine(const std::string& line) {
    RequestLine r;

    // ---- 第 1 处（你来写）：剥掉首尾空白，拿到"干净的一整行" ----
    //
    // 要做的事（3 小步，都在标准库里）：
    //   ① 找第一个"不是空白"的下标
    //   ② 找最后一个"不是空白"的下标
    //   ③ 用 substr 把中间那段取出来
    //
    // ⚠ 两种边界情况都要处理：
    //     · 整行全是空白（或空行）→ 一个都没找到 → 直接 return r（ok 还是 false）
    //     · 找不到时那个函数的返回值是什么？查一下（不是 -1 —— 这一点 CP6c-2 踩过）
    //
    // ⚠ 用 substr 时，"长度"要自己算：末尾下标 - 起始下标 + 1

    // ---- 第 2 处（你来写）：从干净的行里切出三段 ----
    //
    // 要做的事：
    //   ① find 第一个空白 → 它前面那段 = method
    //   ② 从该空白之后继续 find 下一个空白 → 中间那段 = target
    //   ③ 第二个空白之后剩下的全部 = version
    //
    // ⚠ 任何一次 find 找不到（返回 npos），或者切出来的段是空串 → return r
    //    （"GET /" 只有 2 段、"GET / HTTP/1.1 extra" 有 4 段 —— 第二种要在
    //     第 ③ 步里发现：version 里还会夹着空白，想想怎么判）

    // ---- 三段都拿到了：把结果装进 r，并把 ok 置为 true ----
    // ⚠ 下面 4 行现在是【占位】，第 2 处写完后就把它们替换掉
    r.method  = "";
    r.target  = "";
    r.version = "";
    r.ok      = false;
    return r;
}

// ---------------------------------------------------------------------------
// 【靶道】下面这段不用你写 —— 它只负责把结果按固定格式打出来，给测试脚本看：
//     成功 → OK|GET|/a?b=1|HTTP/1.1
//     失败 → ERR
// 手动打一发：
//     printf 'GET / HTTP/1.1' | ./request_line
// ---------------------------------------------------------------------------
int main() {
    std::string line;
    // getline 读一行（读到 '\n' 为止，并且【不把 '\n' 放进 line】）
    std::getline(std::cin, line);

    const RequestLine r = parseRequestLine(line);
    if (!r.ok) {
        std::cout << "ERR" << std::endl;
        return 0;
    }
    std::cout << "OK|" << r.method << "|" << r.target << "|" << r.version << std::endl;
    return 0;
}
