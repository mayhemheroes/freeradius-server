#
# The "dd" command (no relation to Unix/Linux dd or JCL)
#
# Syntax: dd <variable name>
#
# where <variable name> has a type foo * or foo for which
# there is a function foo_debug(FILE *, foo_t *) that presumably
# displays values of type foo_t in a useful, legible format.
#
# When the command is run, it calls the appropriate FreeRADIUS
# function to display the value the variable points at (or
# contains, if it's not a pointer).
#
# Apologia:
#
# Debuggers can print values, but they print them in
# accordance with the C declaration. That's nice, but...
# FreeRADIUS has lots of container types, implemented as
# C's flavor of sum types, i.e. unions with some other
# field used as a tag indicating which variant is in use.
# Debuggers have no way to know which field that is or how
# to interpret its value, so you get to see all those
# variants, only one of which matters.
#
# gdb does support custom printing for data of the type
# of your choice, but
#
# 1. lldb doesn't.
# 2. We already have code to display these structures in a
#    human-friendly form, used for logging. Why not use them?
#
# Both gdb and lldb support Python scripting, and provide
# ways to
#
# 1. Given a string, get a Python value representing it as
#    a variable (or possibly expression) that one can retrieve
#    the type of (and get a textual representation of the type)
# 2. Invoke debugger commands from the Python script.
#
# so we can write Python code to call the FreeRADIUS function
# that displays the value as we want. Since we can tell whether
# we're running under gdb or lldb, we can have a single Python
# source that can do the right thing for the debugger it's running
# under.
#
# NOTE: this function cheerfully assumes such functions exist, but
# will report if the call fails (modulo an issue with lldb and
# fr_value_box_t being defined twice--see below). The idea is that
# these reports will point out where an appropriate foo_debug()
# function needs to exist or to be modified to fit the signature.

import sys

def debug_function(ptr_foo_name):
    # ditch trailng ' *' (will always be there)
    ptr_foo_name = ptr_foo_name[:-2]
    # ditch trailing ' const'
    if ptr_foo_name.endswith(' const'):
        ptr_foo_name = ptr_foo_name[:-6]
    # ditch trailing '_t' (again should always be there)
    if ptr_foo_name.endswith('_t'):
        ptr_foo_name = ptr_foo_name[:-2]
    return f'{ptr_foo_name}_debug'

try:
    import gdb  # Attempt to import GDB module
    dbg = "gdb"
except ImportError:
    import lldb  # If not available, use LLDB module
    dbg = "lldb"

if dbg == "lldb":
    # create LLDB command

    def dd(debugger, command, exe_ctx, result, internal_dict):
        target = debugger.GetSelectedTarget()
        process = target.GetProcess()
        thread = process.GetSelectedThread()
        frame = thread.GetSelectedFrame()
        sb_var = frame.FindVariable(command)
        if not sb_var.IsValid():
            sb_var = target.FindFirstGlobalVariable(command)
        if not sb_var.IsValid():
            result.SetError(f'{command} is not a variable')
            return
        arg = sb_var if sb_var.type.is_pointer else sb_var.address_of
        type = arg.GetDisplayTypeName()
        function = debug_function(type)
        cast = f'({type[:-2]} const *)'
        cmd = f'expr {function}(stderr, {cast}({arg.GetName()}))'
        interpreter = debugger.GetCommandInterpreter()
        if not interpreter.IsValid():
           result.SetError("can't set up SBCommandInterpreter")
           return
        # The use of fr_value_box_t to represent two different structures
        # makes the command fail for it, but only once(!). Until we find
        # the right way to disambiguate, we'll give it up to two tries.
        for i in range(2):
            if (cmdStatus := interpreter.HandleCommand(cmd, result)) == lldb.eReturnStatusSuccessFinishResult:
                return
        result.SetError(f'command "{cmd}" failed, status {cmdStatus}')

    # And then some boilerplate to set up the command and announce its availability.
    # I'm guessing that the -f option is <file name without extension>.<name of function>,
    # and it's followed by the name one uses on the lldb command line.
    def __lldb_init_module(debugger, internal_dict):
        debugger.HandleCommand('command script add -f dd.dd dd')
        print('The "dd" python command has been installed and is ready for use.')

    # To make this available to you in lldb, you need to do this:
    #
    # (lldb) command script import <path to this file>
    #
    # or have it done in your .lldbinit file, which needn't be in your home directory;
    # giving lldb the --local-lldbinit option makes it look in the current directory.
else:
    # create GDB command
    class DD (gdb.Command):
        """Display selected data structures using FreeRADIUS C calls."""
        def __init__ (self):
            super (DD, self).__init__ ("dd", gdb.COMMAND_USER)
            print('The "dd" command has been installed and is ready for use.')
        def invoke (self, arg, from_tty):
            # Python code goes here
            var = gdb.parse_and_eval(arg) # really just sets up for eventual evaluation
            isAddress = var.type.code == gdb.TYPE_CODE_PTR
            if isAddress:
                argMod = ''
            else:
                argMod = '&'
                var = var.address
            varType = str(var.type)
            function = debug_function(varType)
            cast = f'({varType[0:-2]} const *)'
            command = f'call {function}(stderr, {cast}{argMod}({arg}))'
            try:
                gdb.execute(command)
            except:
                print("command failed")
    DD () # Create an instance so you can run it

    # to make this available to you in gdb, execute the command
    #
    # (gdb) source <path to this file>
    #
    # or have that command run when gdb starts, which would involve putting it in
    # the .gdbinit file
