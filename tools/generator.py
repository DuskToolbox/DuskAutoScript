import argparse
import os
import re
from datetime import datetime, timezone

g_debug = True


class InheritTreeNode:
    def __init__(self, name):
        self.name = name
        self.children = []
        self.parents = []

    def add_node(self, node):
        self.children.append(node)
        node.parents.append(self)

    def get_parents(self):
        return self.parents

    def get_name(self):
        return self.name

    def __str__(self) -> str:
        return self.name


class InheritTree:
    def __init__(self, root_name_list):
        self.children_map = {}
        for root_name in root_name_list:
            self.children_map[root_name] = InheritTreeNode(root_name)

    def add_child_to(self, father_name, child_name):
        if child_name not in self.children_map:
            node = InheritTreeNode(child_name)
            self.children_map[child_name] = node
            self.children_map[father_name].add_node(node)
            return
        self.children_map[father_name].add_node(self.children_map[child_name])

    def add_forward_declaration(self, forward_declaration_name):
        if forward_declaration_name not in self.children_map:
            self.children_map[forward_declaration_name] = InheritTreeNode(forward_declaration_name)

    def to_official_iid_list(self):
        result = """const std::unordered_set<DasGuid> g_official_iids{
    []()
    {
        std::unordered_set<DasGuid> result{{"""

        result += '\n'

        keys = sorted(self.children_map.keys())
        for k in keys:
            result += f"            DasIidOf<{k}>(),\n"
        result = result[:-2]
        result += '\n'
        result += """}};
        return result;
    }()};\n"""
        if g_debug:
            print(result)
        return result

    @staticmethod
    def get_inheritance_list(parent_list, state, result_list):
        if len(parent_list) == 0:
            if len(state) == 0:
                return
            result_list.append(state.copy())
            return

        for parent in parent_list:
            state.append(parent)
            InheritTree.get_inheritance_list(parent.get_parents(), state, result_list)
            state.pop()

    def to_preset_type_inheritance_info(self):
        result = ""
        result_list = []
        type_names = sorted(self.children_map.keys())
        for t in type_names:
            result += f"DAS_INTERFACE {t};\n"
        result += '\n'
        for t in self.children_map.values():
            type_parents = t.get_parents()
            if len(type_parents) == 0:
                continue
            child_type = t.get_name()
            state = []
            parent_list = []
            InheritTree.get_inheritance_list(type_parents, state, parent_list)
            if len(parent_list) == 0:
                continue
            # 单继承，只有一个路径
            parents_name = [t.get_name() for t in reversed(parent_list[0])]
            single_result = f"DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO({child_type}"
            for parent_name in parents_name:
                single_result += f", {parent_name}"
            single_result += ");\n"
            result_list.append(single_result)
        result_list = sorted(result_list)
        for r in result_list:
            result += r
        if g_debug:
            print(result)
        return result

    def to_cpp_swig_map(self):
        result = ''
        keys = self.children_map.keys()
        template = 'IDasSwig'
        for k in keys:
            if k.find(template) != -1:
                cpp_type_name = k[len(template):]
                result += f'{{DasIidOf<IDas{cpp_type_name}>(), DasIidOf<{k}>()}},\n'
        result = result[:-2]
        result += '\n'
        if g_debug:
            print(result)
        return result

    def to_cpp_swig_interop_factory(self):
        result = '''
template <class SwigT>
auto CreateCppToSwigObjectImpl(void* p_swig_object, void** pp_out_cpp_object)
    -> DasResult
{
    try
    {
        auto* const p_cpp_object =
            new SwigToCpp<SwigT>(static_cast<SwigT*>(p_swig_object));
        p_cpp_object->AddRef();
        *pp_out_cpp_object = p_cpp_object;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

/**
 * @brief
 * 注意：外部保证传入的指针一定是已经转换到T的指针。如果指针是QueryInterface的返回值，则代表无问题。
 * @tparam T
 * @param p_cpp_object
 * @return
 */
template <class T>
auto CreateSwigToCppObjectImpl(void* p_cpp_object) -> DasRetSwigBase
{
    DasRetSwigBase result{};
    try
    {
        using SwigType = CppToSwig<T>::SwigType;
        auto* const p_swig_object =
            new CppToSwig<T>(static_cast<T*>(p_cpp_object));
        p_swig_object->AddRef();
        result.error_code = DAS_S_OK;
        // explicit 导致要decltype来显式写出类型，似乎没有必要explicit了
        result.value = decltype(result.value){
            static_cast<void*>(static_cast<SwigType*>(p_swig_object))};

        return result;
    }
    catch (const std::bad_alloc&)
    {
        result.error_code = DAS_E_OUT_OF_MEMORY;
        return result;
    }
}

'''
        cpp_to_swig_factory_list = []
        swig_to_cpp_factory_list = []
        template = 'IDasSwig'
        for k in self.children_map.keys():
            if k.find(template) != -1:
                cpp_type_name = k[len(template):]
                cpp_to_swig_factory_list.append(f'''
{{DasIidOf<{k}>(),
    [](void* p_swig_object, void** pp_out_cpp_object)
{{
    return CreateCppToSwigObjectImpl<{k}>(
        p_swig_object,
        pp_out_cpp_object);
}}}},''')
                swig_to_cpp_factory_list.append(f'''
{{DasIidOf<IDas{cpp_type_name}>(), [](void* p_cpp_object)
{{ return CreateSwigToCppObjectImpl<IDas{cpp_type_name}>(p_cpp_object); }}}},''')
        result += '''
std::unordered_map<
    DasGuid,
    DasResult (*)(void* p_swig_object, void** pp_out_cpp_object)>
    g_cpp_to_swig_factory {
'''
        for s in cpp_to_swig_factory_list:
            for l in s.splitlines():
                result += f'        {l}\n'
        result += '\n    };\n'
        result += '''
const std::unordered_map<DasGuid, DasRetSwigBase (*)(void* p_cpp_object)>
    g_swig_to_cpp_factory{
'''
        for s in swig_to_cpp_factory_list:
            for l in s.splitlines():
                result += f'        {l}\n'
        result += '\n    };\n'
        if g_debug:
            print(result)
        return result


def parse_line(line):
    if line.find('DAS_INTERFACE') != -1:
        pattern = r"DAS_INTERFACE\s+(\w+)\s*:\s*public\s+(\w+)"
        match = re.match(pattern, line)
        if match:
            child = match.group(1)
            father = match.group(2)

            print(f"\t找到基类： {father}；找到子类： {child}")

            return [child, father]
        else:
            forward_declare_pattern = r"DAS_INTERFACE\s+(\w+)"
            match = re.match(forward_declare_pattern, line)
            if match:
                value = match.group(1)
                print(f"\t找到前向声明：{value}")
                return [value]
            print(f"\t未匹配到有效数据，输入为： {line}")
    return []


def read_type_in_directory(directory_path):
    type_tree = InheritTree(['IDasBase', 'IDasSwigBase'])

    for root, dirs, files in os.walk(directory_path):
        for filename in files:
            file_path = os.path.join(root, filename)
            if os.path.isfile(file_path):
                print(f"解析文件 {file_path} ...")
                try:
                    with open(file_path, "r", encoding='utf-8') as file:
                        while True:
                            line = file.readline()
                            if not line:
                                break
                            try:
                                pares_line_result = parse_line(line)
                                result_count = len(pares_line_result)
                                if result_count == 0:
                                    continue
                                elif result_count == 1:
                                    father = pares_line_result[0]
                                    type_tree.add_forward_declaration(father)
                                elif result_count == 2:
                                    child, father = pares_line_result[0], pares_line_result[1]
                                    type_tree.add_child_to(father, child)
                            except Exception as e:
                                print(f"\t\t解析行时出错：{e}，行：{line}")

                except Exception as e:
                    print(f"读取文件 {file_path} 时出错: {e}")
            else:
                print(f"跳过 {file_path}")
    return type_tree


def get_all_file_relative_paths(start_directory, request_directory_path):
    result = []
    for root, dirs, files in os.walk(request_directory_path):
        for filename in files:
            file_path = os.path.join(root, filename)
            if os.path.isfile(file_path):
                result.append(os.path.relpath(file_path, start_directory))
    return result


def include_files(files):
    result = ''
    for f in files:
        result += f'#include <{f}>\n'
    result = result.replace("\\", "/")
    return result


def write_file_info(file):
    file.write(
        f'// This file is automatically generated by generator.py at UTC {datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]}.\n// !!! DO NOT EDIT !!!\n')


def mode0(output_path, type_tree, include_file_list):
    cpp_swig_bi_map = type_tree.to_cpp_swig_map()
    # 输出 CppSwigBiMap
    cpp_swig_bi_map_header = open(f'{output_path}/CppSwigBiMap.h', 'w', encoding='utf-8')
    write_file_info(cpp_swig_bi_map_header)
    cpp_swig_bi_map_header.write(
'''
#ifndef DAS_AUTOGEN_CPPSWIGBIMAP_H
#define DAS_AUTOGEN_CPPSWIGBIMAP_H

#include <boost/bimap/bimap.hpp>
#include <boost/bimap/unordered_set_of.hpp>
#include <das/IDasBase.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>

namespace Das{
    namespace _autogen{
        using CppSwigMap = boost::bimaps::bimap<
            // cpp iid
            boost::bimaps::unordered_set_of<DasGuid, std::hash<DasGuid>>,
            // swig iid
            boost::bimaps::unordered_set_of<DasGuid, std::hash<DasGuid>>>;
        extern const CppSwigMap g_cpp_swig_map;
    }
}

#endif // DAS_AUTOGEN_CPPSWIGBIMAP_H
''')
    cpp_swig_bi_map_header.close()

    cpp_swig_bi_map_source = open(f'{output_path}/CppSwigBiMap.cpp', 'w', encoding='utf-8')
    write_file_info(cpp_swig_bi_map_source)
    cpp_swig_bi_map_source.write(include_files(include_file_list))
    cpp_swig_bi_map_source.write(
'''
#include "CppSwigBiMap.h"

namespace Das{
    namespace _autogen{
        const CppSwigMap g_cpp_swig_map = []() -> CppSwigMap
        {
            std::initializer_list<CppSwigMap::value_type> list{
'''
    )
    for l in cpp_swig_bi_map.splitlines():
        cpp_swig_bi_map_source.write(f'                {l}\n')
    cpp_swig_bi_map_source.write(
'''
            };
            return {list.begin(), list.end()};
        }();
    }
}
'''
    )
    cpp_swig_bi_map_source.close()

    # 输出 OfficialIids
    official_iids = type_tree.to_official_iid_list()
    official_iids_header = open(f'{output_path}/OfficialIids.h', 'w', encoding='utf-8')
    write_file_info(official_iids_header)
    official_iids_header.write(
'''
#ifndef DAS_AUTOGEN_IIDLIST_H
#define DAS_AUTOGEN_IIDLIST_H

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <unordered_set>

namespace Das{
    namespace _autogen{
        extern const std::unordered_set<DasGuid> g_official_iids;
    }
}

#endif // DAS_AUTOGEN_IIDLIST_H
'''
    )
    official_iids_header.close()

    official_iids_source = open(f'{output_path}/OfficialIids.cpp', 'w', encoding='utf-8')
    write_file_info(official_iids_source)
    official_iids_source.write(include_files(include_file_list))
    official_iids_source.write(
'''
#include "OfficialIids.h"

namespace Das{
    namespace _autogen{
''')
    for l in official_iids.splitlines():
        official_iids_source.write(f'        {l}\n')
    official_iids_source.write(
'''
    }
}
'''
    )
    official_iids_source.close()
    # ! 由于无法全部实例化这些类型，因此该生成已经被禁用
    # 生成 CppSwigInteropFactory
#     cpp_swig_interop_factory_header = open(f'{output_path}/CppSwigInteropFactory.h', 'w', encoding='utf-8')
#     write_file_info(cpp_swig_interop_factory_header)
#     cpp_swig_interop_factory_header.write('''
# #ifndef DAS_AUTOGEN_CPPSWIGINTEROPFACTORY_H
# #define DAS_AUTOGEN_CPPSWIGINTEROPFACTORY_H

# #include <unordered_map>
# #include <das/Core/ForeignInterfaceHost/DasGuid.h>

# namespace Das{
#     namespace _autogen{
#         extern std::unordered_map<
#             DasGuid,
#             DasResult (*)(void* p_swig_object, void** pp_out_cpp_object)>
#             g_cpp_to_swig_factory;

#         extern const std::unordered_map<DasGuid, DasRetSwigBase (*)(void* p_cpp_object)>
#             g_swig_to_cpp_factory;
#     }
# }

# #endif // DAS_AUTOGEN_CPPSWIGINTEROPFACTORY_H
# ''')
#     cpp_swig_interop_factory_header.close()
#     cpp_swig_interop_factory = type_tree.to_cpp_swig_interop_factory()
#     cpp_swig_interop_factory_source = open(f'{output_path}/CppSwigInteropFactory.cpp', 'w', encoding='utf-8')
#     write_file_info(cpp_swig_interop_factory_source)
#     cpp_swig_interop_factory_source.write(include_files(include_file_list))
#     cpp_swig_interop_factory_source.write('''
# #include <das/Core/Logger/Logger.h>
# #include <das/Core/ForeignInterfaceHost/CppSwigInterop.h>

# using namespace DAS::Core::ForeignInterfaceHost;

# ''')
#     cpp_swig_interop_factory_source.write(
# '''
# #include "CppSwigInteropFactory.h"

# namespace Das{
#     namespace _autogen{
# ''')
#     for l in cpp_swig_interop_factory.splitlines():
#         cpp_swig_interop_factory_source.write(f'        {l}\n')
#     cpp_swig_interop_factory_source.write('''
#     }
# }
# ''')
#     cpp_swig_interop_factory_source.close()
    print('成功生成 CppSwigBiMap 和 OfficialIids')


def mode1(output_path, type_tree):
    preset_type_inheritance_info = type_tree.to_preset_type_inheritance_info()
    preset_type_inheritance_info_virtual_header = ''
    preset_type_inheritance_info_virtual_header  += '// This file is automatically generated by generator.py\n// !!! DO NOT EDIT !!!\n'
    preset_type_inheritance_info_virtual_header  += '''
#ifndef DAS_UTILS_PRESETTYPEINHERITANCEINFO_H
#define DAS_UTILS_PRESETTYPEINHERITANCEINFO_H

#include <das/IDasBase.h>
#include <das/Utils/InternalTypeList.hpp>

DAS_UTILS_NS_BEGIN

template <class T>
struct PresetTypeInheritanceInfo;

'''
    preset_type_inheritance_info_virtual_header  += "#define DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(EndType, ...)             \\"
    preset_type_inheritance_info_virtual_header  += '\n'
    preset_type_inheritance_info_virtual_header  += "DAS_UTILS_NS_BEGIN                                                         \\"
    preset_type_inheritance_info_virtual_header  += '\n'
    preset_type_inheritance_info_virtual_header  += "using EndType##InheritanceInfo =                                           \\"
    preset_type_inheritance_info_virtual_header  += '\n'
    preset_type_inheritance_info_virtual_header  += "    DAS::Utils::internal_type_holder<__VA_ARGS__, EndType>;                \\"
    preset_type_inheritance_info_virtual_header  += '\n'
    preset_type_inheritance_info_virtual_header  += "template <>                                                                \\"
    preset_type_inheritance_info_virtual_header  += '\n'
    preset_type_inheritance_info_virtual_header  += "struct PresetTypeInheritanceInfo<EndType>                                  \\"
    preset_type_inheritance_info_virtual_header  += '\n'
    preset_type_inheritance_info_virtual_header  += "{                                                                          \\"
    preset_type_inheritance_info_virtual_header  += '\n'
    preset_type_inheritance_info_virtual_header  += "    using TypeInfo = EndType##InheritanceInfo;                             \\"
    preset_type_inheritance_info_virtual_header  += '\n'
    preset_type_inheritance_info_virtual_header  += "};                                                                         \\"
    preset_type_inheritance_info_virtual_header  += '\nDAS_UTILS_NS_END'
    preset_type_inheritance_info_virtual_header  += '''

// IDasBase.h
using IDasBaseInheritanceInfo = internal_type_holder<IDasBase>;
template <>
struct PresetTypeInheritanceInfo<::IDasBase>
{
    using TypeInfo = IDasBaseInheritanceInfo;
};

using IDasSwigBaseInheritanceInfo = internal_type_holder<IDasSwigBase>;
template <>
struct PresetTypeInheritanceInfo<::IDasSwigBase>
{
    using TypeInfo = IDasSwigBaseInheritanceInfo;
};

DAS_UTILS_NS_END

'''
    preset_type_inheritance_info_virtual_header  += preset_type_inheritance_info
    preset_type_inheritance_info_virtual_header  += '''
#endif // DAS_UTILS_PRESETTYPEINHERITANCEINFO_H
'''
    preset_type_inheritance_info_header_path = f'{output_path}/PresetTypeInheritanceInfo.h'
    if (os.path.exists(preset_type_inheritance_info_header_path)):
        preset_type_inheritance_info_header = open(preset_type_inheritance_info_header_path, 'r', encoding='utf-8')
        if preset_type_inheritance_info_virtual_header == preset_type_inheritance_info_header.read():
            preset_type_inheritance_info_header.close()
            print('检测到具有预期内容的 PresetTypeInheritanceInfo.h ，跳过生成')
            return

    preset_type_inheritance_info_header = open(preset_type_inheritance_info_header_path, 'w', encoding='utf-8')
    preset_type_inheritance_info_header.write(preset_type_inheritance_info_virtual_header)
    preset_type_inheritance_info_header.close()
    print('成功生成 PresetTypeInheritanceInfo.h')


# ----------------------------------------------------------------

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Read header file and generate code.')
    parser.add_argument('-I', dest='input_value', help='头文件所在文件夹路径')
    parser.add_argument('-O', dest='output_value', help='输出文件夹路径')
    parser.add_argument("--debug", action="store_true", help="启用调试模式")
    parser.add_argument('-M', dest='mode', help='生成器运行模式，可选值为0，1，all')

    args = parser.parse_args()

    g_debug = args.debug

    if args.input_value:
        print(f'输入的头文件所在文件夹路径: {args.input_value}')
    else:
        print('-I无输入！退出！')
        exit(1)

    if args.output_value:
        print(f'输出文件夹路径: {args.output_value}')
    else:
        print('-O无输入！退出！')
        exit(2)

    output_path = args.output_value
    input_path = args.input_value

    # 检查output是否存在，不存在就创建一下，避免linux下后续创建文件失败
    if not os.path.exists(output_path):
        os.makedirs(output_path)

    # input_path = r'D:\Sources\CPP\OpenSourceProjects\AutoStarRail\include\AutoStarRail'
    # output_path = r'D:\Sources\CPP\OpenSourceProjects\AutoStarRail\build\DAS\include\DAS\_autogen'

    type_tree = read_type_in_directory(input_path)

    include_file_list = get_all_file_relative_paths(f'{input_path}/../', input_path)

    mode = ''
    if args.mode:
        mode = args.mode
    else:
        mode = 'all'
    if mode == '0':
        mode0(output_path, type_tree, include_file_list)
    elif mode == '1':
        mode1(output_path, type_tree)
    elif mode == 'all':
        mode0(output_path, type_tree, include_file_list)
        mode1(output_path, type_tree)
    print(f'处理模式：{mode}\n---------------------------------')

