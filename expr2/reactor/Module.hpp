// Copyright 2021 The Reactor Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//	http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "Reactor.hpp"

#include <vector>

#ifndef rr_Module_hpp
#	define rr_Module_hpp

namespace llvm {
	class Function;
}

namespace rr {

// Module constructs a reactor Module that comprises of multiple functions.
// The last function will be the main function required by acquire.
//
// Example usage:
//
//   // Build the coroutine function
//   Module module;
//   ModuleFunction<Int(Int)> helper(module, "helperfunction");
//   helper.setPure(); // this is pure function
//   {
//       Int x = helper.Arg<0>();
//       Return(x * 42);
//   }
//   ModuleFunction<Int(Int)> main(module);
//   {
//       Int x = helper.Arg<0>();
//       Int y = helper.Call(x);
//       Return(y + 42);
//   }
//   auto routine = module.acquire("main");
//
//
class Module
{
	std::vector<llvm::Function *> functions;
	std::unique_ptr<Nucleus> core;
public:
	Module() : core(new Nucleus()) {}

	//Nucleus *getCore() { return core.get(); }
	void add(llvm::Function *f, const char *name);

	std::shared_ptr<Routine> acquire(const char *name, const Config::Edit &cfgEdit = Config::Edit::None);
};

// Internal use only.
Value *Call(llvm::Function *func, std::initializer_list<Value *> args);
void setPure(llvm::Function *func);

// Generic template, leave undefined!
template<typename FunctionType>
class ModuleFunction;

// Specialized for function types
template<typename Return, typename... Arguments>
class ModuleFunction<Return(Arguments...)>
{
	// Static assert that the function signature is valid.
	static_assert(sizeof(AssertFunctionSignatureIsValid<Return(Arguments...)>) >= 0, "Invalid function signature");

public:
	ModuleFunction(Module &m, const char *name = nullptr);

	template<int index>
	Argument<typename std::tuple_element<index, std::tuple<Arguments...>>::type> Arg() const
	{
		Value *arg = Nucleus::getArgument(index);
		return Argument<typename std::tuple_element<index, std::tuple<Arguments...>>::type>(arg);
	}

	ModuleFunction &setPure() { rr::setPure(func); return *this; }

	RValue<Return> Call(RValue<Arguments> ...args)
	{
		return As<Return>(rr::Call(func, { std::forward<Value *>(args.value())... }));
	}

protected:
	Module *mod;
	Type *retType;
	std::vector<Type *> argTypes;
	llvm::Function *func;
};

template<typename Return>
class ModuleFunction<Return()> : public ModuleFunction<Return(Void)>
{
};

template<typename Return, typename... Arguments>
ModuleFunction<Return(Arguments...)>::ModuleFunction(Module &m, const char *name /* = nullptr*/)
{
	mod = &m;
	retType = Return::type();
	Type *types[] = { Arguments::type()... };
	for(Type *type : types)
	{
		if(type != Void::type())
		{
			argTypes.push_back(type);
		}
	}
	Nucleus::createFunction(Return::type(), argTypes);
	m.add(func = static_cast<llvm::Function *>(Nucleus::getLastFunction()), name);
}


}  // namespace rr

#endif  // rr_Module_hpp
