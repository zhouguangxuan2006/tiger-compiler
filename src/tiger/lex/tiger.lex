%filenames = "scanner"

/* 你可以在这里添加 Lex 定义 */
digit [0-9]
letter [a-zA-Z]

/* 定义启动条件（状态） */
/* COMMENT 用于处理嵌套注释，STR 用于处理字符串，IGNORE 用于处理字符串中的 \f...f\ 忽略部分 */
%x COMMENT STR IGNORE

%%

 /* * 1. 保留字 (Reserved Words) 
  * 参考 Tiger 语言手册，返回对应的枚举值 [cite: 33, 34]
  */
"array"    {adjust(); return Parser::ARRAY;}
"if"       {adjust(); return Parser::IF;}
"then"     {adjust(); return Parser::THEN;}
"else"     {adjust(); return Parser::ELSE;}
"while"    {adjust(); return Parser::WHILE;}
"for"      {adjust(); return Parser::FOR;}
"to"       {adjust(); return Parser::TO;}
"do"       {adjust(); return Parser::DO;}
"let"      {adjust(); return Parser::LET;}
"in"       {adjust(); return Parser::IN;}
"end"      {adjust(); return Parser::END;}
"of"       {adjust(); return Parser::OF;}
"break"    {adjust(); return Parser::BREAK;}
"nil"      {adjust(); return Parser::NIL;}
"function" {adjust(); return Parser::FUNCTION;}
"var"      {adjust(); return Parser::VAR;}
"type"     {adjust(); return Parser::TYPE;}

 /* * 2. 标点符号与操作符 
  */
","   {adjust(); return Parser::COMMA;}
":"   {adjust(); return Parser::COLON;}
";"   {adjust(); return Parser::SEMICOLON;}
"("   {adjust(); return Parser::LPAREN;}
")"   {adjust(); return Parser::RPAREN;}
"["   {adjust(); return Parser::LBRACK;}
"]"   {adjust(); return Parser::RBRACK;}
"{"   {adjust(); return Parser::LBRACE;}
"}"   {adjust(); return Parser::RBRACE;}
"."   {adjust(); return Parser::DOT;}
"+"   {adjust(); return Parser::PLUS;}
"-"   {adjust(); return Parser::MINUS;}
"*"   {adjust(); return Parser::TIMES;}
"/"   {adjust(); return Parser::DIVIDE;}
"="   {adjust(); return Parser::EQ;}
"<>"  {adjust(); return Parser::NEQ;}
"<"   {adjust(); return Parser::LT;}
"<="  {adjust(); return Parser::LE;}
">"   {adjust(); return Parser::GT;}
">="  {adjust(); return Parser::GE;}
"&"   {adjust(); return Parser::AND;}
"|"   {adjust(); return Parser::OR;}
":="  {adjust(); return Parser::ASSIGN;}

 /* * 3. 标识符与整数 
  */
{digit}+  {adjust(); return Parser::INT;}
{letter}({letter}|{digit}|_)* {adjust(); return Parser::ID;}

 /* * 4. 注释处理 (支持嵌套) [cite: 37, 41, 42]
  */
"/*" {
    adjust();
    comment_level_++; // 注意：需在 scanner.h 中定义 int comment_level_ = 0;
    begin(StartCondition_::COMMENT);
}

<COMMENT>{
    "/*" {adjust(); comment_level_++;}
    "*/" {
        adjust();
        if (--comment_level_ == 0) begin(StartCondition_::INITIAL);
    }
    \n   {adjust(); errormsg_->Newline();}
    .    {adjust();}
}

 /* * 5. 字符串处理 
  */
\" {
    adjust();
    string_buf_.clear(); // 注意：需在 scanner.h 中定义 std::string string_buf_;
    begin(StartCondition_::STR);
}

<STR>{
    \" {
        adjustStr();
        // 关键：不要在这里给 string_buf_ 加东西，直接结束
        setMatched(string_buf_); 
        begin(StartCondition_::INITIAL);
        return Parser::STRING;
    }
    
    /* 1. 处理控制字符 \^X (例如 \^G) */
    "\\^"[@A-Z\[\\\]\^_] {
        adjustStr();
        string_buf_ += (char)(matched()[2] - '@');
    }

    /* 2. 处理标准转义 */
    "\\n"  {adjustStr(); string_buf_ += '\n';}
    "\\t"  {adjustStr(); string_buf_ += '\t';}
    "\\\"" {adjustStr(); string_buf_ += '\"';}
    "\\\\" {adjustStr(); string_buf_ += '\\';}

    /* 3. 处理 \ddd ASCII 码 */
    \\[0-9]{3} {
        adjustStr();
        int code = std::stoi(matched().substr(1));
        string_buf_ += (char)code;
    }

    /* 4. 处理 \f...f\ 忽略格式 */
    "\\" {
        adjustStr(); 
        begin(StartCondition_::IGNORE);
    }

    /* 5. 处理字符串内的普通换行（取决于测试集要求，通常需记录行号） */
    \n {
        adjustStr(); 
        errormsg_->Newline(); 
        string_buf_ += '\n';
    }

    /* 6. 其他普通字符 */
    . {
        adjustStr(); 
        string_buf_ += matched();
    }
}

<IGNORE>{
    "\\"    {adjustStr(); begin(StartCondition_::STR);}
    \n      {adjustStr(); errormsg_->Newline();}
    [ \t\f] {adjustStr();}
}

 /* * 6. 其他辅助规则 
  */
[ \t]+ {adjust();}
\n     {adjust(); errormsg_->Newline();}

 /* 非法字符处理 [cite: 19, 22, 110] */
. {adjust(); errormsg_->Error(errormsg_->tok_pos_, "illegal token");}