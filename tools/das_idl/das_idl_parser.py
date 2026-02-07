"""
DAS IDL 解析器
解析类 IDL 语法文件，生成结构化的接口定义

支持的语法:
    [uuid("xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx")]
    interface IInterfaceName : IBaseInterface {
# 方法定义
        DasResult MethodName([out] IOutputType** pp_out);
        
# 属性定义 (自动生成 getter/setter)
        [get, set] int PropertyName
        [get] DasReadOnlyString ReadOnlyProperty
    }

# 枚举定义
    enum EnumName {
        Value1 = 0,
        Value2 = 1,
    }
"""

import re
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import Optional


class TokenType(Enum):
    """词法分析的 Token 类型"""
    UUID = auto()
    INTERFACE = auto()
    ENUM = auto()
    STRUCT = auto()
    NAMESPACE = auto()  # 添加 namespace 支持
    IMPORT = auto()  # 添加 import 支持
    IDENTIFIER = auto()
    LBRACE = auto()
    RBRACE = auto()
    LPAREN = auto()
    RPAREN = auto()
    LBRACKET = auto()
    RBRACKET = auto()
    COLON = auto()
    SEMICOLON = auto()
    COMMA = auto()
    EQUALS = auto()
    STAR = auto()
    AMPERSAND = auto()
    STRING = auto()
    NUMBER = auto()
    COMMENT = auto()
    NEWLINE = auto()
    EOF = auto()


@dataclass
class Token:
    """词法分析的 Token"""
    type: TokenType
    value: str
    line: int
    column: int


class ParamDirection(Enum):
    """参数方向"""
    IN = "in"
    OUT = "out"
    INOUT = "inout"


@dataclass
class TypeInfo:
    """类型信息"""
    base_type: str          # 基础类型名称
    is_pointer: bool = False  # 是否是指针
    pointer_level: int = 0    # 指针层级
    is_const: bool = False    # 是否是 const
    is_reference: bool = False  # 是否是引用


@dataclass
class ParameterDef:
    """方法参数定义"""
    name: str
    type_info: TypeInfo
    direction: ParamDirection = ParamDirection.IN
    namespace: str = ""  # 添加命名空间支持


@dataclass
class MethodDef:
    """方法定义"""
    name: str
    return_type: TypeInfo
    parameters: list = field(default_factory=list)
    is_pure_virtual: bool = True
    namespace: str = ""  # 添加命名空间支持
    attributes: dict = field(default_factory=dict)  # 方法属性（如 binary_buffer）


@dataclass
class PropertyDef:
    """属性定义 (用于自动生成 getter/setter)"""
    name: str
    type_info: TypeInfo
    has_getter: bool = True
    has_setter: bool = True
    namespace: str = ""  # 添加命名空间支持


@dataclass
class StructFieldDef:
    """结构体字段定义"""
    name: str
    type_name: str  # 字段类型（必须是基本类型）
    namespace: str = ""  # 添加命名空间支持


@dataclass
class StructDef:
    """结构体定义"""
    name: str
    fields: list = field(default_factory=list)  # StructFieldDef 列表
    namespace: str = ""  # 添加命名空间支持


@dataclass
class EnumValueDef:
    """枚举值定义"""
    name: str
    value: Optional[int] = None
    namespace: str = ""  # 添加命名空间支持


@dataclass
class EnumDef:
    """枚举定义"""
    name: str
    values: list = field(default_factory=list)
    namespace: str = ""  # 添加命名空间支持


@dataclass
class InterfaceDef:
    """接口定义"""
    name: str
    uuid: str
    base_interface: str = "IDasBase"
    methods: list = field(default_factory=list)
    properties: list = field(default_factory=list)
    is_swig_exported: bool = False
    namespace: str = ""  # 添加命名空间支持


@dataclass
class ImportDef:
    """导入定义"""
    idl_path: str  # 导入的 IDL 文件路径（相对路径）
    line: int = 0  # 导入语句所在行号（用于错误提示）


@dataclass
class IdlDocument:
    """IDL 文档"""
    interfaces: list = field(default_factory=list)
    enums: list = field(default_factory=list)
    structs: list = field(default_factory=list)  # 结构体定义列表
    includes: list = field(default_factory=list)
    imports: list = field(default_factory=list)  # 导入的其他 IDL 文件
    namespace: str = ""  # 添加文档级别的命名空间


class Lexer:
    """词法分析器"""
    
    KEYWORDS = {
        'interface': TokenType.INTERFACE,
        'enum': TokenType.ENUM,
        'struct': TokenType.STRUCT,
        'namespace': TokenType.NAMESPACE,  # 添加 namespace 关键字
        'import': TokenType.IMPORT,  # 添加 import 关键字
    }
    
    def __init__(self, source: str):
        self.source = source
        self.pos = 0
        self.line = 1
        self.column = 1
        self.tokens = []
        
    def current_char(self) -> Optional[str]:
        if self.pos >= len(self.source):
            return None
        return self.source[self.pos]
    
    def peek(self, offset: int = 1) -> Optional[str]:
        pos = self.pos + offset
        if pos >= len(self.source):
            return None
        return self.source[pos]
    
    def advance(self) -> str:
        char = self.current_char()
        self.pos += 1
        if char == '\n':
            self.line += 1
            self.column = 1
        else:
            self.column += 1
        return char
    
    def skip_whitespace(self):
        while self.current_char() and self.current_char() in ' \t\r':
            self.advance()
    
    def skip_line_comment(self):
        while self.current_char() and self.current_char() != '\n':
            self.advance()
    
    def skip_block_comment(self):
        self.advance()  # 跳过 *
        while self.current_char():
            if self.current_char() == '*' and self.peek() == '/':
                self.advance()  # 跳过 *
                self.advance()  # 跳过 /
                return
            self.advance()
    
    def read_string(self) -> str:
        quote = self.advance()  # 跳过开始引号
        result = []
        while self.current_char() and self.current_char() != quote:
            if self.current_char() == '\\':
                self.advance()
                if self.current_char():
                    result.append(self.advance())
            else:
                result.append(self.advance())
        if self.current_char() == quote:
            self.advance()  # 跳过结束引号
        return ''.join(result)
    
    def read_identifier(self) -> str:
        result = []
        while self.current_char() and (self.current_char().isalnum() or self.current_char() == '_'):
            result.append(self.advance())
        return ''.join(result)
    
    def read_number(self) -> str:
        result = []
        # 支持十六进制
        if self.current_char() == '0' and self.peek() in 'xX':
            result.append(self.advance())  # 0
            result.append(self.advance())  # x
            while self.current_char() and (self.current_char().isdigit() or self.current_char() in 'abcdefABCDEF'):
                result.append(self.advance())
        else:
            # 支持负数
            if self.current_char() == '-':
                result.append(self.advance())
            while self.current_char() and self.current_char().isdigit():
                result.append(self.advance())
        return ''.join(result)
    
    def tokenize(self) -> list:
        """执行词法分析"""
        while self.current_char():
            char = self.current_char()
            
            # 跳过空白
            if char in ' \t\r':
                self.skip_whitespace()
                continue
            
            # 换行
            if char == '\n':
                self.advance()
                continue
            
            # 注释
            if char == '/':
                if self.peek() == '/':
                    self.advance()
                    self.advance()
                    self.skip_line_comment()
                    continue
                elif self.peek() == '*':
                    self.advance()
                    self.skip_block_comment()
                    continue
            
            # 字符串
            if char in '"\'':
                line, col = self.line, self.column
                value = self.read_string()
                self.tokens.append(Token(TokenType.STRING, value, line, col))
                continue
            
            # 数字
            if char.isdigit() or (char == '-' and self.peek() and self.peek().isdigit()):
                line, col = self.line, self.column
                value = self.read_number()
                self.tokens.append(Token(TokenType.NUMBER, value, line, col))
                continue
            
            # 标识符和关键字
            if char.isalpha() or char == '_':
                line, col = self.line, self.column
                value = self.read_identifier()
                token_type = self.KEYWORDS.get(value, TokenType.IDENTIFIER)
                self.tokens.append(Token(token_type, value, line, col))
                continue
            
            # 单字符 Token
            single_char_tokens = {
                '{': TokenType.LBRACE,
                '}': TokenType.RBRACE,
                '(': TokenType.LPAREN,
                ')': TokenType.RPAREN,
                '[': TokenType.LBRACKET,
                ']': TokenType.RBRACKET,
                ':': TokenType.COLON,
                ';': TokenType.SEMICOLON,
                ',': TokenType.COMMA,
                '=': TokenType.EQUALS,
                '*': TokenType.STAR,
                '&': TokenType.AMPERSAND,
            }
            
            if char in single_char_tokens:
                line, col = self.line, self.column
                self.tokens.append(Token(single_char_tokens[char], char, line, col))
                self.advance()
                continue
            
            # 未知字符，跳过
            self.advance()
        
        self.tokens.append(Token(TokenType.EOF, '', self.line, self.column))
        return self.tokens


class Parser:
    """语法分析器"""
    
    # 内置类型映射
    BUILTIN_TYPES = {
        'void', 'bool', 'int', 'int8', 'int16', 'int32', 'int64',
        'uint8', 'uint16', 'uint32', 'uint64', 'float', 'double',
        'size_t', 'DasResult', 'DasBool', 'DasGuid', 'DasString',
        'DasReadOnlyString', 'IDasReadOnlyString', 'char',
        'unsigned char', 'unsigned int', 'unsigned short', 'unsigned long',
        'signed char', 'signed int', 'signed short', 'signed long'
    }
    
    # 复合类型关键字（如 unsigned char）
    COMPOUND_TYPE_PREFIXES = {'unsigned', 'signed'}
    
    # struct 中允许使用的基本类型
    STRUCT_BASIC_TYPES = {
        'bool', 'int', 'int8', 'int16', 'int32', 'int64',
        'uint8', 'uint16', 'uint32', 'uint64', 'float', 'double',
        'char', 'size_t',
        # 带 _t 后缀的 C/C++ 标准类型
        'int8_t', 'int16_t', 'int32_t', 'int64_t',
        'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t',
        # 复合类型
        'unsigned char', 'unsigned int', 'unsigned short', 'unsigned long',
        'signed char', 'signed int', 'signed short', 'signed long'
    }
    
    def __init__(self, tokens: list):
        self.tokens = tokens
        self.pos = 0
        self.document = IdlDocument()
        self._current_namespace = ""  # 添加当前命名空间跟踪
        self._namespaces = set()  # 收集所有遇到的命名空间，用于校验
        
    def current(self) -> Token:
        if self.pos >= len(self.tokens):
            return self.tokens[-1]
        return self.tokens[self.pos]
    
    def peek(self, offset: int = 1) -> Token:
        pos = self.pos + offset
        if pos >= len(self.tokens):
            return self.tokens[-1]
        return self.tokens[pos]
    
    def advance(self) -> Token:
        token = self.current()
        self.pos += 1
        return token
    
    def expect(self, token_type: TokenType, value: Optional[str] = None) -> Token:
        token = self.current()
        if token.type != token_type:
            raise SyntaxError(f"Expected {token_type}, got {token.type} at line {token.line}")
        if value and token.value != value:
            raise SyntaxError(f"Expected '{value}', got '{token.value}' at line {token.line}")
        return self.advance()
    
    def match(self, token_type: TokenType, value: Optional[str] = None) -> bool:
        token = self.current()
        if token.type != token_type:
            return False
        if value and token.value != value:
            return False
        return True
    
    def parse(self) -> IdlDocument:
        """解析整个文档"""
        while not self.match(TokenType.EOF):
            # 解析属性 (如 [uuid("...")])
            attributes = {}
            if self.match(TokenType.LBRACKET):
                attributes = self.parse_attributes()
            
            # 根据关键字解析不同的定义
            if self.match(TokenType.IMPORT):
                import_def = self.parse_import()
                self.document.imports.append(import_def)
            elif self.match(TokenType.NAMESPACE):
                self.parse_namespace()
            elif self.match(TokenType.INTERFACE):
                interface = self.parse_interface(attributes)
                self.document.interfaces.append(interface)
            elif self.match(TokenType.ENUM):
                enum = self.parse_enum()
                self.document.enums.append(enum)
            elif self.match(TokenType.STRUCT):
                struct = self.parse_struct()
                self.document.structs.append(struct)
            else:
            # 跳过未知内容
                self.advance()
        
        # 校验：一个 IDL 文件中应该只有一个命名空间
        self._validate_single_namespace()
        
        return self.document
    
    def parse_import(self) -> ImportDef:
        """解析 import 语句
        
        支持的语法:
            import "path/to/file.idl";
            import path/to/file.idl;  // 也支持不带引号的形式
        """
        token = self.expect(TokenType.IMPORT)
        line = token.line
        
        # 解析导入路径
        if self.match(TokenType.STRING):
            # import "path/to/file.idl";
            idl_path = self.advance().value
        elif self.match(TokenType.IDENTIFIER):
            # import path/to/file.idl; (无引号形式)
            # 收集路径的所有部分
            parts = [self.advance().value]
            # 继续收集路径（处理 / 和 . 分隔符）
            while self.current().value in ('/', '.', '_') or self.match(TokenType.IDENTIFIER):
                if self.current().value in ('/', '.', '_'):
                    parts.append(self.advance().value)
                elif self.match(TokenType.IDENTIFIER):
                    parts.append(self.advance().value)
                else:
                    break
            idl_path = ''.join(parts)
        else:
            raise SyntaxError(f"Expected import path at line {self.current().line}")
        
        # 期望分号结束
        self.expect(TokenType.SEMICOLON)
        
        return ImportDef(idl_path=idl_path, line=line)
    
    def parse_attributes(self) -> dict:
        """解析属性列表 [attr1, attr2("value")]"""
        self.expect(TokenType.LBRACKET)
        attributes = {}

        # UUID格式正则表达式：xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
        UUID_PATTERN = re.compile(r'^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$')

        while not self.match(TokenType.RBRACKET) and not self.match(TokenType.EOF):
            name = self.expect(TokenType.IDENTIFIER).value

            # 检查是否有参数
            if self.match(TokenType.LPAREN):
                self.advance()
                if self.match(TokenType.STRING):
                    value = self.advance().value
                    
                    # 如果是uuid属性，进行格式校验
                    if name == 'uuid':
                        if not UUID_PATTERN.match(value):
                            raise SyntaxError(
                                f"Invalid UUID format: '{value}'. Expected: 8-4-4-4-12 hex chars (e.g., 123e4567-e89b-12d3-a456-426614174000)"
                            )
                    
                    attributes[name] = value
                elif self.match(TokenType.NUMBER):
                    attributes[name] = self.advance().value
                elif self.match(TokenType.IDENTIFIER):
                    attributes[name] = self.advance().value
                self.expect(TokenType.RPAREN)
            else:
                attributes[name] = True

            # 逗号分隔
            if self.match(TokenType.COMMA):
                self.advance()

        self.expect(TokenType.RBRACKET)
        return attributes
    
    def parse_namespace(self):
        """解析 namespace 定义，支持 C++17 嵌套命名空间语法"""
        self.expect(TokenType.NAMESPACE)
        
        # 解析命名空间名称（支持 A::B::C 格式）
        namespace_parts = []
        namespace_parts.append(self.expect(TokenType.IDENTIFIER).value)
        
        # 检查是否有 :: 嵌套
        while self.match(TokenType.COLON):
            self.advance()  # 消费 :
            self.expect(TokenType.COLON)  # 消费第二个 :
            namespace_parts.append(self.expect(TokenType.IDENTIFIER).value)
        
        namespace_name = "::".join(namespace_parts)
        self.expect(TokenType.LBRACE)
        
        # 保存之前的命名空间
        old_namespace = self._current_namespace
        
        # 设置新的命名空间（支持嵌套）
        if self._current_namespace:
            self._current_namespace = f"{self._current_namespace}::{namespace_name}"
        else:
            self._current_namespace = namespace_name
        
        # 记录命名空间（记录完整的第一级命名空间定义）
        # 例如：namespace DAS::FirstNamespace 记录为 "DAS::FirstNamespace"
        # 而不是只记录 "DAS"
        self._namespaces.add(self._current_namespace)
        
        # 解析 namespace 内的内容
        while not self.match(TokenType.RBRACE) and not self.match(TokenType.EOF):
            # 解析属性 (如 [uuid("...")])
            attributes = {}
            if self.match(TokenType.LBRACKET):
                attributes = self.parse_attributes()
            
            # 根据关键字解析不同的定义
            if self.match(TokenType.NAMESPACE):
                self.parse_namespace()
            elif self.match(TokenType.INTERFACE):
                interface = self.parse_interface(attributes)
                self.document.interfaces.append(interface)
            elif self.match(TokenType.ENUM):
                enum = self.parse_enum()
                self.document.enums.append(enum)
            elif self.match(TokenType.STRUCT):
                struct = self.parse_struct()
                self.document.structs.append(struct)
            else:
                # 跳过未知内容
                self.advance()
        
        if self.match(TokenType.EOF):
            raise SyntaxError(f"Unexpected end of file while parsing namespace '{namespace_name}'")
        
        self.expect(TokenType.RBRACE)
        
        # 恢复之前的命名空间
        self._current_namespace = old_namespace
    
    def parse_interface(self, attributes: dict) -> InterfaceDef:
        """解析接口定义"""
        self.expect(TokenType.INTERFACE)
        name = self.expect(TokenType.IDENTIFIER).value
        
        # 获取 UUID
        uuid = attributes.get('uuid', '')
        
        # 解析基类
        base_interface = "IDasBase"
        if self.match(TokenType.COLON):
            self.advance()
            # 检查是否是非法关键字（C++ 访问修饰符、存储类说明符、虚函数说明符等）
            ILLEGAL_BASE_CLASS_KEYWORDS = {'public', 'private', 'protected', 'virtual', 'static', 'const', 'volatile'}
            next_token = self.current()
            if next_token.type == TokenType.IDENTIFIER and next_token.value in ILLEGAL_BASE_CLASS_KEYWORDS:
                raise SyntaxError(
                    f"Interface base class cannot use C++ access modifier '{next_token.value}' at line {next_token.line}. "
                    f"Expected: interface identifier (e.g., IDasBase, IDasTypeInfo). "
                    f"Note: IDL interface inheritance does not support C++ modifiers like 'public'."
                )
            base_interface = self.expect(TokenType.IDENTIFIER).value

        
        interface = InterfaceDef(
            name=name,
            uuid=uuid,
            base_interface=base_interface,
            namespace=self._current_namespace  # 设置命名空间
        )
        
        self.expect(TokenType.LBRACE)
        
        # 解析接口体
        while not self.match(TokenType.RBRACE) and not self.match(TokenType.EOF):
            # 检查是否有属性
            member_attrs = {}
            if self.match(TokenType.LBRACKET):
                member_attrs = self.parse_attributes()
            
            # 判断是属性还是方法
            if 'get' in member_attrs or 'set' in member_attrs:
                prop = self.parse_property(member_attrs)
                interface.properties.append(prop)
            else:
                method = self.parse_method(member_attrs)
                interface.methods.append(method)
        
        if self.match(TokenType.EOF):
            raise SyntaxError(f"Unexpected end of file while parsing interface '{name}'")
        
        self.expect(TokenType.RBRACE)
        
            # 可选的分号
        if self.match(TokenType.SEMICOLON):
            self.advance()
        
        return interface
    
    def parse_type(self) -> TypeInfo:
        """解析类型

        支持复合类型如:
            unsigned char
            signed int
            const unsigned char*

        支持命名空间限定符:
            Das::ExportInterface::DasDate*
        """
        is_const = False

        # 检查 const
        if self.match(TokenType.IDENTIFIER) and self.current().value == 'const':
            is_const = True
            self.advance()

        # 基础类型（支持复合类型如 unsigned char）
        base_type = self.expect(TokenType.IDENTIFIER).value

        # 支持命名空间限定符（如 Das::ExportInterface::DasDate）
        while self.match(TokenType.COLON):
            # 跳过第一个冒号
            self.advance()
            # 确保是第二个冒号
            self.expect(TokenType.COLON)
            # 读取下一个标识符
            next_part = self.expect(TokenType.IDENTIFIER).value
            base_type = f"{base_type}::{next_part}"

        # 如果是复合类型前缀（unsigned/signed），继续读取下一个标识符
        if base_type in self.COMPOUND_TYPE_PREFIXES:
            if self.match(TokenType.IDENTIFIER):
                next_type = self.advance().value
                base_type = f"{base_type} {next_type}"

        # 检查指针和引用
        pointer_level = 0
        is_reference = False

        while True:
            if self.match(TokenType.STAR):
                pointer_level += 1
                self.advance()
            elif self.match(TokenType.AMPERSAND):
                is_reference = True
                self.advance()
            else:
                break

        return TypeInfo(
            base_type=base_type,
            is_pointer=pointer_level > 0,
            pointer_level=pointer_level,
            is_const=is_const,
            is_reference=is_reference
        )
    
    def parse_method(self, attributes: dict) -> MethodDef:
        """解析方法定义"""
        # 返回类型
        return_type = self.parse_type()
        
        # 方法名
        name = self.expect(TokenType.IDENTIFIER).value
        
        # 参数列表
        self.expect(TokenType.LPAREN)
        parameters = []
        
        while not self.match(TokenType.RPAREN) and not self.match(TokenType.EOF):
            param = self.parse_parameter()
            parameters.append(param)
            
            if self.match(TokenType.COMMA):
                self.advance()
        
        if self.match(TokenType.EOF):
            raise SyntaxError(f"Unexpected end of file while parsing method '{name}'")
        
        self.expect(TokenType.RPAREN)
        
        # 检查纯虚函数标记
        is_pure_virtual = True
        if self.match(TokenType.EQUALS):
            self.advance()
            self.expect(TokenType.NUMBER)  # = 0
        
        # 分号
        if self.match(TokenType.SEMICOLON):
            self.advance()
        
        return MethodDef(
            name=name,
            return_type=return_type,
            parameters=parameters,
            is_pure_virtual=is_pure_virtual,
            namespace=self._current_namespace,  # 设置命名空间
            attributes=attributes  # 保存方法属性
        )
    
    def parse_parameter(self) -> ParameterDef:
        """解析参数"""
        direction = ParamDirection.IN
        
        # 检查方向属性 [in], [out], [inout]
        if self.match(TokenType.LBRACKET):
            attrs = self.parse_attributes()
            if 'out' in attrs:
                direction = ParamDirection.OUT
            elif 'inout' in attrs:
                direction = ParamDirection.INOUT
        
        # 类型
        type_info = self.parse_type()
        
        # 参数名
        name = self.expect(TokenType.IDENTIFIER).value
        
        return ParameterDef(
            name=name,
            type_info=type_info,
            direction=direction,
            namespace=self._current_namespace  # 设置命名空间
        )
    
    def parse_property(self, attributes: dict) -> PropertyDef:
        """解析属性定义"""
        has_getter = 'get' in attributes
        has_setter = 'set' in attributes
        
        # 类型
        type_info = self.parse_type()
        
        # 属性名
        name = self.expect(TokenType.IDENTIFIER).value
        
        # 可选分号
        if self.match(TokenType.SEMICOLON):
            self.advance()
        
        return PropertyDef(
            name=name,
            type_info=type_info,
            has_getter=has_getter,
            has_setter=has_setter,
            namespace=self._current_namespace  # 设置命名空间
        )
    
    def parse_enum(self) -> EnumDef:
        """解析枚举定义"""
        self.expect(TokenType.ENUM)
        name = self.expect(TokenType.IDENTIFIER).value
        
        enum = EnumDef(name=name, namespace=self._current_namespace) # 设置命名空间
        
        self.expect(TokenType.LBRACE)
        
        current_value = 0
        while not self.match(TokenType.RBRACE) and not self.match(TokenType.EOF):
            value_name = self.expect(TokenType.IDENTIFIER).value
            
            # 检查是否有显式值
            value = None
            if self.match(TokenType.EQUALS):
                self.advance()
                value_str = self.expect(TokenType.NUMBER).value
                if value_str.startswith('0x') or value_str.startswith('0X'):
                    value = int(value_str, 16)
                else:
                    value = int(value_str)
                current_value = value
            else:
                value = current_value
            
            enum.values.append(EnumValueDef(name=value_name, value=value, namespace=self._current_namespace))  # 设置命名空间
            current_value += 1
            
            # 逗号分隔
            if self.match(TokenType.COMMA):
                self.advance()
        
        if self.match(TokenType.EOF):
            raise SyntaxError(f"Unexpected end of file while parsing enum '{name}'")
        
        self.expect(TokenType.RBRACE)
        
        # 可选分号
        if self.match(TokenType.SEMICOLON):
            self.advance()
        
        return enum
    
    def parse_struct(self) -> StructDef:
        """解析结构体定义
        
        支持的语法:
            struct StructName {
                double field1;
                int field2;
            };
        
        注意: 字段类型必须是基本类型，否则报错
        """
        self.expect(TokenType.STRUCT)
        name = self.expect(TokenType.IDENTIFIER).value
        
        struct = StructDef(name=name, namespace=self._current_namespace)
        
        self.expect(TokenType.LBRACE)
        
        while not self.match(TokenType.RBRACE) and not self.match(TokenType.EOF):
            # 解析字段类型
            type_token = self.expect(TokenType.IDENTIFIER)
            type_name = type_token.value
            
            # 验证类型必须是基本类型
            if type_name not in self.STRUCT_BASIC_TYPES:
                raise SyntaxError(
                    f"struct '{name}' 的字段类型 '{type_name}' 不是基本类型。"
                    f"允许的基本类型: {', '.join(sorted(self.STRUCT_BASIC_TYPES))}。"
                    f"位于第 {type_token.line} 行"
                )
            
            # 解析字段名
            field_name = self.expect(TokenType.IDENTIFIER).value
            
            # 期望分号
            self.expect(TokenType.SEMICOLON)
            
            struct.fields.append(StructFieldDef(
                name=field_name,
                type_name=type_name,
                namespace=self._current_namespace
            ))
        
        if self.match(TokenType.EOF):
            raise SyntaxError(f"Unexpected end of file while parsing struct '{name}'")
        
        self.expect(TokenType.RBRACE)
        
        # 可选分号
        if self.match(TokenType.SEMICOLON):
            self.advance()
        
        return struct
    
    def _validate_single_namespace(self):
        """校验：一个 IDL 文件中应该只有一个命名空间"""
        if len(self._namespaces) > 1:
            ns_list = ", ".join(f'"{ns}"' for ns in sorted(self._namespaces))
            raise ValueError(
                f"IDL 文件只能包含一个命名空间，但发现了 {len(self._namespaces)} 个: {ns_list}"
            )
        
        # 如果没有命名空间但有接口或枚举，发出警告
        if not self._namespaces and (self.document.interfaces or self.document.enums):
            # 注意：这里只是记录警告，不抛出异常，因为可能存在无命名空间的旧文件
            pass


def parse_idl(source: str) -> IdlDocument:
    """解析 IDL 源代码"""
    lexer = Lexer(source)
    tokens = lexer.tokenize()
    parser = Parser(tokens)
    return parser.parse()


def parse_idl_file(file_path: str) -> IdlDocument:
    """解析 IDL 文件"""
    with open(file_path, 'r', encoding='utf-8') as f:
        source = f.read()
    return parse_idl(source)


# 测试代码
if __name__ == '__main__':
    test_idl = '''
    // 测试枚举
    enum DasTestEnum {
        Value1 = 0,
        Value2,
        Value3 = 100,
    }
    
    // 测试接口
    [uuid("12345678-1234-1234-1234-123456789abc")]
    interface IDasTest : IDasBase {
        DasResult DoSomething([out] IDasTestResult** pp_out);
        DasResult Process(const DasGuid& guid, int32 count);
    }
    
    // 带属性的接口
    [uuid("87654321-4321-4321-4321-cba987654321")]
    interface IDasTestResult : IDasBase {
        [get, set] int32 Id
        [get] DasReadOnlyString Name
        [set] float Value
    }
    '''
    
    doc = parse_idl(test_idl)
    
    print("=== 枚举 ===")
    for enum in doc.enums:
        print(f"enum {enum.name}:")
        for val in enum.values:
            print(f"  {val.name} = {val.value}")
    
    print("\n=== 接口 ===")
    for iface in doc.interfaces:
        print(f"interface {iface.name} : {iface.base_interface}")
        print(f"  UUID: {iface.uuid}")
        print("  Methods:")
        for method in iface.methods:
            params = ", ".join(f"{p.type_info.base_type}{'*'*p.type_info.pointer_level} {p.name}" for p in method.parameters)
            print(f"    {method.return_type.base_type} {method.name}({params})")
        print("  Properties:")
        for prop in iface.properties:
            accessors = []
            if prop.has_getter:
                accessors.append("get")
            if prop.has_setter:
                accessors.append("set")
            print(f"    [{', '.join(accessors)}] {prop.type_info.base_type} {prop.name}")
