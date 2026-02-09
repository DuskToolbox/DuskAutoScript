#!/usr/bin/env python3
"""
Test script to verify SwigInterfaceModel integration
"""
import sys
import os

# Add tools directory to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools', 'das_idl'))

from das_idl_parser import parse_idl, InterfaceDef
from swig_api_model import (
    build_swig_interface_model,
    build_interface_map,
    SwigInterfaceModel,
    OutParamInfo
)
from swig_lang_generator_base import SwigLangGenerator, SwigLangGeneratorContext
from das_swig_generator import SwigCodeGenerator


class TestLangGenerator(SwigLangGenerator):
    """Test language generator to verify extension points are called"""
    
    def __init__(self):
        super().__init__()
        self.received_model = None
        self.received_interface = None
        self.emit_called = False
    
    def get_language_name(self) -> str:
        return "test"
    
    def get_swig_define(self) -> str:
        return "SWIGTEST"
    
    def generate_out_param_wrapper(self, interface, method, param):
        return ""
    
    def generate_binary_buffer_helpers(self, interface, method_name, size_method_name):
        return ""
    
    def on_interface_model(self, model, interface_def):
        """Verify extension point: receive model"""
        self.received_model = model
        self.received_interface = interface_def
        print(f"  [OK] on_interface_model() called: {model.name}")
        print(f"    - inherits_idas_type_info: {model.inherits_idas_type_info}")
        print(f"    - out_methods: {len(model.out_methods)}")
        print(f"    - multi_out_methods: {len(model.multi_out_methods)}")
    
    def emit_post_include(self, model, interface_def):
        """Verify extension point: emit post-include code"""
        self.emit_called = True
        print(f"  [OK] emit_post_include() called: {model.name}")
        return ""


def test_integration():
    """Test complete integration flow"""
    print("=" * 60)
    print("SwigInterfaceModel Integration Test")
    print("=" * 60)
    
    # Test IDL
    test_idl = '''
    namespace DAS {
        // Test interface - inherits IDasBase
        [uuid("12345678-1234-1234-1234-123456789abc")]
        interface IDasTest : IDasBase {
            // out parameter method
            DasResult GetValue([out] int32* p_out);
            DasResult GetInterface([out] IDasTestResult** pp_out);
            
            // multi out parameter method
            DasResult GetMultiple([out] int32* p_a, [out] int32* p_b);
            
            // binary_buffer method
            [binary_buffer] DasResult GetBuffer([out] unsigned char** pp_buffer, [out] size_t* p_size);
            
            // string parameter method
            DasResult SetName(IDasReadOnlyString* p_name);
            
            // normal method
            DasResult DoSomething(int32 value);
        }
        
        // Test interface - inherits IDasTypeInfo
        [uuid("87654321-4321-4321-4321-cba987654321")]
        interface IDasTypeInfoTest : IDasTypeInfo {
            [get] int32 Id
            [set] IDasReadOnlyString Name
        }
    }
    '''
    
    # Parse IDL
    print("\n[1] Parsing test IDL...")
    doc = parse_idl(test_idl)
    print(f"  [OK] Parsed {len(doc.interfaces)} interfaces")
    
    # Create test generator
    print("\n[2] Creating test language generator...")
    test_generator = TestLangGenerator()
    
    # Create main generator
    print("\n[3] Creating SwigCodeGenerator...")
    generator = SwigCodeGenerator(
        document=doc,
        idl_file_name="Test.idl",
        idl_file_path="test/Test.idl",
        lang_generators=[test_generator],
        debug=True
    )
    
    # Generate interface files
    print("\n[4] Testing interface generation...")
    for interface in doc.interfaces:
        print(f"\n  Interface: {interface.name}")
        i_content = generator.generate_interface_i_file(interface)
        
        # Verify extension points were called
        if test_generator.received_model:
            assert test_generator.received_model.name == interface.name
            print(f"    [OK] Model name matches: {test_generator.received_model.name}")
        else:
            print(f"    [FAIL] Model not received!")
            return False
        
        if test_generator.emit_called:
            print(f"    [OK] emit_post_include called")
        else:
            print(f"    [FAIL] emit_post_include not called!")
            return False
        
        # Reset state
        test_generator.received_model = None
        test_generator.emit_called = False
    
    print("\n" + "=" * 60)
    print("All tests passed! Integration verified successfully")
    print("=" * 60)
    return True


def test_backward_compatibility():
    """Test backward compatibility - ensure existing code is not affected"""
    print("\n" + "=" * 60)
    print("Backward Compatibility Test")
    print("=" * 60)
    
    # Test generator using old generate_post_include_directives
    class OldStyleGenerator(SwigLangGenerator):
        def __init__(self):
            super().__init__()
            self.old_method_called = False
        
        def get_language_name(self) -> str:
            return "oldstyle"
        
        def get_swig_define(self) -> str:
            return "SWIGOLD"
        
        def generate_out_param_wrapper(self, interface, method, param):
            return ""
        
        def generate_binary_buffer_helpers(self, interface, method_name, size_method_name):
            return ""
        
        def generate_post_include_directives(self, interface):
            # Old style method (should still work)
            self.old_method_called = True
            return ""
        
        # Note: Not overriding on_interface_model and emit_post_include
        # They should use default empty implementations
    
    test_idl = '''
    [uuid("12345678-1234-1234-1234-123456789abc")]
    interface IDasSimple : IDasBase {
        DasResult DoSomething();
    }
    '''
    
    print("\n[1] Testing old-style generator (not overriding new extension points)...")
    doc = parse_idl(test_idl)
    old_gen = OldStyleGenerator()
    generator = SwigCodeGenerator(
        document=doc,
        lang_generators=[old_gen]
    )
    
    i_content = generator.generate_interface_i_file(doc.interfaces[0])
    
    if old_gen.old_method_called:
        print("  [OK] Old generate_post_include_directives still works")
    else:
        print("  [FAIL] Old method not called!")
        return False
    
    print("\n[2] Verifying new extension points default implementations do not throw...")
    print("  [OK] No exceptions thrown")
    
    print("\n" + "=" * 60)
    print("Backward compatibility test passed")
    print("=" * 60)
    return True


if __name__ == '__main__':
    success = True
    
    try:
        success = test_integration() and success
    except Exception as e:
        print(f"\n[FAIL] Integration test failed: {e}")
        import traceback
        traceback.print_exc()
        success = False
    
    try:
        success = test_backward_compatibility() and success
    except Exception as e:
        print(f"\n[FAIL] Compatibility test failed: {e}")
        import traceback
        traceback.print_exc()
        success = False
    
    if success:
        print("\n" + "=" * 60)
        print("All tests passed!")
        print("=" * 60)
        sys.exit(0)
    else:
        print("\n" + "=" * 60)
        print("Tests failed!")
        print("=" * 60)
        sys.exit(1)
