#pragma once
#include <cctype>
#include <string>
#include <string_view>
namespace keryx::nexus {
// Protobuf Struct stores JSON numbers as doubles. Preserve large integral
// lexemes before parsing, without changing strings, floats or invalid JSON.
inline std::string preserve_json_integers(std::string_view input) {
  std::string out;out.reserve(input.size());
  for(std::size_t i=0;i<input.size();) {
    if(input[i]=='"') {
      out+=input[i++];
      while(i<input.size()) {
        const char c=input[i++];out+=c;
        if(c=='\\'&&i<input.size())out+=input[i++];
        else if(c=='"')break;
      }
      continue;
    }
    if(input[i]!='-' && (input[i]<'0'||input[i]>'9')){out+=input[i++];continue;}
    const auto start=i;
    if(input[i]=='-')++i;
    const auto digits=i;
    while(i<input.size()&&input[i]>='0'&&input[i]<='9')++i;
    const auto digits_end=i;
    bool integral=true;
    if(i<input.size()&&input[i]=='.'){integral=false;++i;while(i<input.size()&&input[i]>='0'&&input[i]<='9')++i;}
    if(i<input.size()&&(input[i]=='e'||input[i]=='E')){
      integral=false;++i;if(i<input.size()&&(input[i]=='+'||input[i]=='-'))++i;
      while(i<input.size()&&input[i]>='0'&&input[i]<='9')++i;
    }
    if(i==start){out+=input[i++];continue;}
    const auto number=input.substr(start,i-start),integer=input.substr(digits,digits_end-digits);
    auto next=i;while(next<input.size()&&std::isspace(static_cast<unsigned char>(input[next])))++next;
    const bool valid=!integer.empty()&&!(integer.size()>1&&integer.front()=='0')&&
      !(next<input.size()&&input[next]==':');
    const bool large=integer.size()>16||(integer.size()==16&&integer>std::string_view("9007199254740991"));
    if(integral&&valid&&large){out+='"';out+=number;out+='"';}else out+=number;
  }
  return out;
}
}
