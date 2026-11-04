#include "app.h"
#include <algorithm>

namespace tec {
namespace {
// Match the normal decimal spelling of one octet. A wildcard never crosses a dot.
bool matches(const std::wstring& pattern, unsigned n) {
    const auto digits = std::to_wstring(n);
    std::array<bool,4> match{true};
    for (auto c : pattern) {
        std::array<bool,4> next{};
        if (c == L'*') next[0] = match[0];
        for (size_t i=1; i<=digits.size(); ++i)
            next[i] = c == L'*' ? (match[i] || next[i-1]) : (match[i-1] && (c==L'?' || c==digits[i-1]));
        match = next;
    }
    return match[digits.size()];
}
}

bool expandNetworks(const std::vector<std::wstring>& input, std::vector<std::wstring>& output, std::wstring& error) {
    std::vector<std::wstring> prefixes;
    for (const auto& text : input) {
        error = L"허용 주소를 확인하세요: " + text;
        if (text.find_first_of(L"*?") == text.npos) {
            int family; unsigned bits; std::array<unsigned char,16> bytes{};
            if (!parsePrefix(text,family,bytes,bits) || !bits) return false;
            prefixes.push_back(text);
        } else {
            std::array<std::array<bool,256>,4> allowed{};
            std::array<bool,4> all{};
            size_t begin=0;
            if (text.size()>64) return false;
            for (int i=0; i<4; ++i) {
                const auto end=text.find(L'.',begin);
                if ((i==3)!=(end==text.npos)) return false;
                auto octet=text.substr(begin,end-begin);
                if (octet.empty() || octet.find_first_not_of(L"0123456789*?")!=octet.npos) return false;
                unsigned count=0;
                for (unsigned n=0; n<256; ++n) if ((allowed[i][n]=matches(octet,n))) ++count;
                if (!count) return false;
                all[i]=count==256;
                begin=end==text.npos ? text.size() : end+1;
            }
            int last=3;
            while (last>=0 && all[last]) --last;
            if (last<0) { error=L"전체 인터넷을 허용하는 주소 범위는 사용할 수 없습니다."; return false; }
            std::function<bool(int,std::wstring)> emit = [&](int i,std::wstring base) {
                for (unsigned n=0; n<256;) {
                    if (!allowed[i][n]) { ++n; continue; }
                    if (i<last) {
                        if (!emit(i+1,base+std::to_wstring(n)+L".")) return false;
                        ++n; continue;
                    }
                    unsigned width=1, bits=(i+1)*8;
                    while (width<256 && n%(width*2)==0 && n+width*2<=256 &&
                           std::all_of(allowed[i].begin()+n+width,allowed[i].begin()+n+2*width,[](bool b){return b;})) {
                        width*=2; --bits;
                    }
                    std::wstring address=base+std::to_wstring(n);
                    for (int j=i+1;j<4;++j) address+=L".0";
                    prefixes.push_back(address+L"/"+std::to_wstring(bits)); n+=width;
                    if (prefixes.size()>512) return false;
                }
                return true;
            };
            if (!emit(0,L"")) { error=L"범위가 너무 복잡합니다. 512개 이내의 CIDR로 표현되는 패턴을 입력하세요."; return false; }
        }
        if (prefixes.size()>512) { error=L"허용 범위가 512개를 넘습니다."; return false; }
    }
    output=std::move(prefixes); error.clear(); return true;
}
}
