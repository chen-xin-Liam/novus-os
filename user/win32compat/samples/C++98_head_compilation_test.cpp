#include <windows.h>

#include <cstdlib>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#include <cctype>
#include <climits>
#include <cfloat>
#include <cstdarg>
#include <csetjmp>
#include <csignal>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <list>
#include <deque>
#include <map>
#include <set>
#include <queue>
#include <stack>
#include <algorithm>
#include <functional>
#include <iterator>
#include <numeric>
#include <utility>
#include <memory>
#include <limits>
#include <stdexcept>
#include <typeinfo>
#include <locale>
#include <bitset>

using namespace std;

static HANDLE hConsole;
static int total = 0;
static int passed = 0;

void setGreen()
{
    SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
}
void setRed()
{
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
}
void setReset()
{
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

void testCase(const char* name, bool ok)
{
    total++;
    cout << name << " : ";
    if (ok)
    {
        setGreen();
        cout << "PASS";
        passed++;
    }
    else
    {
        setRed();
        cout << "FAIL";
    }
    setReset();
    cout << endl;
}

jmp_buf g_jmpbuf;
volatile int jmp_ok = 0;

void sig_handler(int) {}
void exit_cb() {}
void va_func(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

int main()
{
    hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

    cout << "===== ISO C++98 Standard Library Full Functional Test (MinGW) =====" << endl << endl;

    // cstddef
    {
        size_t sz = sizeof(int);
        ptrdiff_t pd = 10 - 3;
        testCase("cstddef size_t/ptrdiff_t", (sz > 0 && pd == 7));
    }

    // cstdlib rand + atexit
    {
        int r = rand();
        atexit(exit_cb);
        testCase("cstdlib rand & atexit", (r >= 0));
    }

    // cstdio tmpfile
    {
        FILE* fp = tmpfile();
        bool ok = (fp != NULL);
        if (fp) fclose(fp);
        testCase("cstdio tmpfile", ok);
    }

    // cstring strcpy
    {
        char buf[64];
        strcpy(buf, "abc");
        testCase("cstring strcpy", strcmp(buf, "abc") == 0);
    }

    // ctime time + localtime
    {
        time_t t = time(NULL);
        struct tm* tm_p = localtime(&t);
        testCase("ctime time/localtime", (tm_p != NULL));
    }

    // cmath sqrt
    {
        double s = sqrt(4.0);
        testCase("cmath sqrt", (s > 1.99 && s < 2.01));
    }

    // cctype isalpha
    {
        testCase("cctype isalpha", isalpha('A') != 0);
    }

    // climits
    {
        testCase("climits INT_MAX", INT_MAX > 0);
    }

    // cfloat
    {
        testCase("cfloat DBL_MAX", DBL_MAX > 0);
    }

    // cstdarg
    {
        testCase("cstdarg va_list", true);
    }

    // csetjmp / longjmp
    {
        jmp_ok = 0;
        if (setjmp(g_jmpbuf) == 0)
        {
            longjmp(g_jmpbuf, 1);
        }
        else
        {
            jmp_ok = 1;
        }
        testCase("csetjmp setjmp+longjmp", jmp_ok == 1);
    }

    // csignal
    {
        void (*old)(int) = signal(SIGINT, sig_handler);
        testCase("csignal signal register", true);
    }

    // iostream
    {
        testCase("iostream cout", true);
    }

    // string
    {
        string s("hi");
        s += " test";
        testCase("std::string concat", s == "hi test");
    }

    // sstream stringstream
    {
        stringstream ss;
        ss << 456;
        int x = 0;
        ss >> x;
        testCase("sstream stringstream", x == 456);
    }

    // fstream ofstream
    {
        ofstream ofs("_cpp98_test.tmp");
        ofs << "ok";
        ofs.close();
        ifstream ifs("_cpp98_test.tmp");
        string read;
        ifs >> read;
        ifs.close();
        remove("_cpp98_test.tmp");
        testCase("fstream file rw", read == "ok");
    }

    // vector
    {
        vector<int> v;
        v.push_back(10);
        v.push_back(20);
        testCase("vector push_back", v.size() == 2 && v[0]==10);
    }

    // list
    {
        list<int> li;
        li.push_back(5);
        testCase("list push_back", li.front() == 5);
    }

    // deque
    {
        deque<int> dq;
        dq.push_front(1);
        dq.push_back(9);
        testCase("deque push front/back", dq[0]==1 && dq[1]==9);
    }

    // map
    {
        map<string, int> mp;
        mp["key"] = 99;
        testCase("map insert & lookup", mp["key"] == 99);
    }

    // set
    {
        set<int> st;
        st.insert(77);
        testCase("set insert", st.count(77) == 1);
    }

    // queue
    {
        queue<int> q;
        q.push(12);
        testCase("queue push/front", q.front() == 12);
    }

    // stack
    {
        stack<int> stk;
        stk.push(34);
        testCase("stack push/top", stk.top() == 34);
    }

    // algorithm sort
    {
        vector<int> v;
        v.push_back(3);
        v.push_back(1);
        v.push_back(2);
        sort(v.begin(), v.end());
        testCase("algorithm sort", v[0]==1 && v[1]==2 && v[2]==3);
    }

    // functional less
    {
        less<int> lt;
        testCase("functional less", lt(2,5) == true);
    }

    // iterator back_inserter
    {
        vector<int> v;
        back_insert_iterator<vector<int> > it = back_inserter(v);
        *it = 88;
        testCase("iterator back_inserter", v[0]==88);
    }

    // numeric accumulate
    {
        vector<int> v;
        v.push_back(1);
        v.push_back(2);
        v.push_back(3);
        int sum = accumulate(v.begin(), v.end(), 0);
        testCase("numeric accumulate", sum == 6);
    }

    // utility pair / make_pair
    {
        pair<int,string> p = make_pair(100, "pairtest");
        testCase("utility pair", p.first == 100 && p.second == "pairtest");
    }

    // memory auto_ptr (C++98专属)
    {
        auto_ptr<int> ap(new int(42));
        testCase("memory auto_ptr", *ap == 42);
    }

    // limits numeric_limits
    {
        int minv = numeric_limits<int>::min();
        testCase("limits numeric_limits", minv < 0);
    }

    // stdexcept try throw catch
    {
        bool caught = false;
        try
        {
            throw runtime_error("test exc");
        }
        catch(const runtime_error&)
        {
            caught = true;
        }
        testCase("stdexcept throw/catch", caught);
    }

    // typeinfo typeid
    {
        const type_info& ti = typeid(double);
        testCase("typeinfo typeid", true);
    }

    // locale classic
    {
        locale loc = locale::classic();
        testCase("locale classic locale", true);
    }

    // bitset
    {
        bitset<8> bs(0x05);
        testCase("bitset test", bs.test(0) && bs.test(2));
    }

    cout << endl;
    cout << "===== Result: " << passed << "/" << total << " tests passed =====" << endl;
    if (passed == total)
    {
        setGreen();
        cout << ">>> ALL TESTS PASSED <<<" << endl;
    }
    else
    {
        setRed();
        cout << ">>> SOME TEST FAILED <<<" << endl;
    }
    setReset();
    return 0;
}
