#pragma once

static void CppProxyPrintForwardDecl(IDiaSymbol *global, ResolvedUdtGraphPtr resolved, UdtGraphPtr graph)
{
	std::unordered_set<std::wstring> uniq;

	int count = 0;
	for (auto& node : resolved->nodes) {
		for (auto& idep : node->dep_s) {

			if (endsWith(idep.first, L'*') && IsAllowedName(idep.first.c_str()) && !startsWith(idep.first, L"void")) {

				std::wstring name(idep.first);
				replace(name, L"*", L"");
				trim(name);

				auto inode = graph->nodes.find(name);
				if (inode != graph->nodes.end()) {

					auto fixedName(fixName(name));
					if (!fixedName.empty() && uniq.find(fixedName) == uniq.end()) {
						uniq.insert(fixedName);

						DWORD kind;
						inode->second->symbol->get_udtKind(&kind);

						wprintf(L"%s %s;\n", rgUdtKind[kind], fixedName.c_str());
						++count;
					}
				}
			}
		}
	}
	if (count) {
		wprintf(L"\n");
	}
}

static void CppProxyPrintUDT(IDiaSymbol *pUDT, BOOL bGuardObject = FALSE, BOOL bZeroInitialize = FALSE)
{
	// https://github.com/MicrosoftDocs/visualstudio-docs/blob/master/docs/debugger/debug-interface-access/lexical-hierarchy-of-symbol-types.md
	// https://github.com/MicrosoftDocs/visualstudio-docs/blob/master/docs/debugger/debug-interface-access/class-hierarchy-of-symbol-types.md
	// https://github.com/MicrosoftDocs/visualstudio-docs/blob/master/docs/debugger/debug-interface-access/udt.md

	DWORD symTag;
	if (pUDT->get_symTag(&symTag) != S_OK) return;
	if (symTag != SymTagUDT) return;

	DWORD kind = 0;
	pUDT->get_udtKind(&kind);
	if (!(kind == UdtStruct || kind == UdtClass))
	{
		return;
	}

	Bstr name;
	if (pUDT->get_name(&name) != S_OK) {
		return;
	}

	// class <NAME>
	std::wstring uname(*name);
	std::wstring nameFixed(fixName(uname));
	std::wstring namePrefix;
	std::wstring nameInner;
	{
		size_t pos = uname.find_last_of(L"::");
		if (pos != std::string::npos)
		{
			namePrefix.assign(uname.substr(0, pos - 1));
			nameInner.assign(uname.substr(pos + 1));
		}
		else
		{
			namePrefix.clear();
			nameInner.assign(uname);
		}
	}

	wprintf(L"%s %s", rgUdtKind[kind], nameFixed.c_str());

	// base class
	{
		int baseId = 0;
		SymbolEnumerator symbol;
		if (symbol.Find(pUDT, SymTagBaseClass, NULL)) {
			while (symbol.Next()) {

				if (!baseId) wprintf(L" : ");
				else wprintf(L", ");
				wprintf(L"public ");

				Bstr baseName;
				symbol->get_name(&baseName);

				std::wstring baseNameFixed(fixName(*baseName));
				wprintf(L"%s", baseNameFixed.c_str());
				baseId++;
			}
		}
	}

	// body
	wprintf(L" {\n");
	wprintf(L"public:\n");

	BOOL addFakeVtp = FALSE;

	// virtual table ptr
	{
		BOOL hasIntroVf = FALSE;
		BOOL hasOverVf = FALSE;

		SymbolEnumerator symbol;
		if (symbol.Find(pUDT, SymTagFunction, NULL)) {
			while (symbol.Next()) {

				Bstr funcName;
				symbol->get_name(&funcName);

				BOOL isVirtual = FALSE;
				symbol->get_virtual(&isVirtual);

				if (isVirtual) {
					BOOL isIntro = FALSE;
					symbol->get_intro(&isIntro);

					if (isIntro)
						hasIntroVf = TRUE;
					else
						hasOverVf = TRUE;
				}
			}
		}

		if (hasIntroVf && !hasOverVf)
		{
			addFakeVtp = TRUE;
			wprintf(L"\tvoid* _vtable;\n");
		}
	}

	// data
	{
		SymbolEnumerator symbol;
		if (symbol.Find(pUDT, SymTagData, NULL)) {
			while (symbol.Next()) {

				DWORD dwDataKind;
				symbol->get_dataKind(&dwDataKind);

				if (dwDataKind == DataIsMember)
				{
					Bstr fieldName;
					symbol->get_name(&fieldName);

					ComRef<IDiaSymbol> fieldType;
					symbol->get_type(&fieldType);

					DWORD fieldTypeTag;
					fieldType->get_symTag(&fieldTypeTag);

					BOOL isRef = FALSE;
					fieldType->get_reference(&isRef);

					wprintf(L"\t");
					if (!isRef) {
						PrintTypeX(*fieldType);
					}
					else {
						PrintPointerTypeX(*fieldType, NULL, TRUE);
					}
					wprintf(L" %s", *fieldName);

					if (fieldTypeTag == SymTagArrayType)
					{
						PrintArraySizeX(*fieldType);
					}

					// default zero initialization
					if (bZeroInitialize && (fieldTypeTag == SymTagBaseType || fieldTypeTag == SymTagPointerType || fieldTypeTag == SymTagEnum || fieldTypeTag == SymTagArrayType))
					{
						if (!isRef)
						{
							if (fieldTypeTag == SymTagEnum)
							{
								// SomeEnum e = {} crashes the compiler LOL
								wprintf(L" = (");
								PrintTypeX(*fieldType);
								wprintf(L")(0)");
							}
							else if (fieldTypeTag == SymTagArrayType)
							{
								ComRef<IDiaSymbol> arrType;
								fieldType->get_type(&arrType);

								DWORD arrTypeTag;
								arrType->get_symTag(&arrTypeTag);

								if (arrTypeTag == SymTagBaseType || arrTypeTag == SymTagPointerType)
								{
									wprintf(L" = {}");
								}
							}
							else
								wprintf(L" = 0");
						}
					}

					wprintf(L";\n");
				}
			}
		}
	}

	// methods
	{
		BOOL hasDefConstructor = FALSE;
		BOOL hasCtor = FALSE;
		BOOL hasDtor = FALSE;

		std::wstring ctorPrefix;
		ctorPrefix.assign(L"__cdecl ");
		ctorPrefix.assign(nameInner);
		ctorPrefix.append(L"::");
		ctorPrefix.append(nameInner);
		ctorPrefix.append(L"(");

		std::wstring fixedFuncName;
		fixedFuncName.reserve(1024);

		std::wstring funcNamespace(*name);
		funcNamespace.append(L"::");

		SymbolEnumerator symbol;
		if (symbol.Find(pUDT, SymTagFunction, NULL)) {
			while (symbol.Next()) {

				DWORD funcId;
				symbol->get_symIndexId(&funcId);

				Bstr funcName;
				symbol->get_name(&funcName);
				if (wcsstr(*funcName, L"__vecDelDtor")) continue;

				fixedFuncName.assign(*funcName);
				replace(fixedFuncName, funcNamespace, L"");

				Bstr undecName;
				symbol->get_undecoratedName(&undecName);

				BOOL isDtor = (wcsstr(*funcName, L"~") ? TRUE : FALSE);
				BOOL isCtor = (wcscmp(*funcName, nameInner.c_str()) == 0);

				// HACK for newer msdia
				if (wcsstr(*undecName, ctorPrefix.c_str()))
				{
					isCtor = TRUE;
				}

				BOOL isFunc = !(isCtor || isDtor);

				BOOL isPure = FALSE;
				symbol->get_pure(&isPure);

				BOOL isVirtual = FALSE;
				symbol->get_virtual(&isVirtual);

				BOOL isStatic = FALSE;
				symbol->get_isStatic(&isStatic);

				ULONGLONG len = 0;
				DWORD dwLocType = 0, dwRVA = 0, dwSect = 0, dwOff = 0;
				symbol->get_length(&len);
				symbol->get_locationType(&dwLocType);
				symbol->get_relativeVirtualAddress(&dwRVA);
				symbol->get_addressSection(&dwSect);
				symbol->get_addressOffset(&dwOff);
				BOOL isOptimized = (dwLocType == 0);

				// HACK for newer msdia
				if (isFunc && dwLocType == LocIsStatic && dwRVA && isPure && !isVirtual)
				{
					isStatic = TRUE;
				}

				BOOL isValidVirtual = FALSE;
				DWORD vtpo = 0; // virtual table pointer offset
				DWORD vfid = 0; // virtual function id in VT

				if (isVirtual) {
					isValidVirtual = GetVirtualFuncInfo(pUDT, *symbol, vtpo, vfid);
					if (!isValidVirtual) {
						wprintf(L"\t#error INVALID VFID %s::%s\n", nameFixed.c_str(), *funcName);
					}
				}

				DWORD callConv = 0;
				symbol->get_callingConvention(&callConv); // rgCallConv[callConv]

				ComRef<IDiaSymbol> funcType;
				symbol->get_type(&funcType);

				ComRef<IDiaSymbol> returnType;
				funcType->get_type(&returnType);

				// UDT ctor/dtor
				
				if (isCtor && !isOptimized) {
					hasCtor = TRUE;
					wprintf(L"\tinline %s * ctor(", nameFixed.c_str());
					PrintFunctionArgsX(*symbol, TRUE, TRUE);
					wprintf(L") {");

					wprintf(L" typedef ");
					wprintf(L"%s *", nameFixed.c_str());
					wprintf(L" (%s::*_fpt)(", nameFixed.c_str());
					PrintFunctionArgsX(*symbol, TRUE, FALSE);
					wprintf(L");");
					
					wprintf(L" auto _f=xcast<_fpt>(_drva(%u));", (unsigned int)dwRVA);
					wprintf(L" return (this->*_f)(");
					PrintFunctionArgsX(*symbol, FALSE, TRUE);
					wprintf(L");");

					wprintf(L" }\n");
				}

				if (isDtor && (!isOptimized || (isVirtual && isValidVirtual))) {
					hasDtor = TRUE;
					wprintf(L"\tinline void dtor() {");

					wprintf(L" typedef ");
					PrintTypeX(*returnType);
					wprintf(L" (%s::*_fpt)(", nameFixed.c_str());
					PrintFunctionArgsX(*symbol, TRUE, FALSE);
					wprintf(L");");

					if (isVirtual) {
						wprintf(L" auto _f=xcast<_fpt>(get_vfp(this, %u));", (unsigned int)vfid);
					}
					else {
						wprintf(L" auto _f=xcast<_fpt>(_drva(%u));", (unsigned int)dwRVA);
					}
					wprintf(L" (this->*_f)();");

					wprintf(L" }\n");
				}

				// direct rva calls

				if (isFunc && !isOptimized) {

					wprintf(L"\tinline ");
					if (isStatic) {
						wprintf(L"static ");
					}
					PrintTypeX(*returnType);
					wprintf(L" ");
					if (callConv > 0) {
						wprintf(L"%s ", rgCallConv[callConv]);
					}
					if (isVirtual) {
						wprintf(L"%s_impl", fixedFuncName.c_str()); // funcName
					}
					else {
						wprintf(L"%s", fixedFuncName.c_str()); // funcName
					}
					wprintf(L"(");
					PrintFunctionArgsX(*symbol, TRUE, TRUE);
					wprintf(L")");

					// body
					wprintf(L" { ");

					wprintf(L"typedef ");
					PrintTypeX(*returnType);
					wprintf(L" (");
					if (callConv > 0) {
						wprintf(L"%s ", rgCallConv[callConv]);
					}
					if (!isStatic) {
						wprintf(L"%s::", nameFixed.c_str());
					}
					wprintf(L"*_fpt)(");
					if (isStatic) {
						PrintFunctionArgsX(*symbol, TRUE, FALSE);
					}
					else {
						PrintFunctionArgsX(*symbol, TRUE, FALSE);
					}
					wprintf(L");");

					if (isStatic) {
						wprintf(L" auto _f=(_fpt)_drva(%u);", (unsigned int)dwRVA);
					}
					else {
						wprintf(L" auto _f=xcast<_fpt>(_drva(%u));", (unsigned int)dwRVA);
					}

					if (isStatic) {
						wprintf(L" return _f(");
					}
					else {
						wprintf(L" return (this->*_f)(");
					}
					if (isStatic) {
						PrintFunctionArgsX(*symbol, FALSE, TRUE);
					}
					else {
						PrintFunctionArgsX(*symbol, FALSE, TRUE);
					}
					wprintf(L");");

					wprintf(L" }");
					wprintf(L"\n");
				}

				// virtual redirectors

				#if defined(GEN_VFUNC_REDIR)
				if (isVirtual && isFunc) {
					wprintf(L"\tinline ");
					PrintTypeX(*returnType);
					wprintf(L" %s(", fixedFuncName.c_str()); // *funcName
					PrintFunctionArgsX(*symbol, TRUE, TRUE);
					wprintf(L") {");

					wprintf(L" typedef ");
					PrintTypeX(*returnType);
					wprintf(L" (%s::*_fpt)(", nameFixed.c_str());
					PrintFunctionArgsX(*symbol, TRUE, FALSE);
					wprintf(L");");
					wprintf(L" auto _f=xcast<_fpt>(get_vfp(this, %u));", (unsigned int)vfid);
					wprintf(L" return (this->*_f)(");
					PrintFunctionArgsX(*symbol, FALSE, TRUE);
					wprintf(L");");

					wprintf(L" }\n");
				}
				#endif
			}
		}

		if (!hasCtor) {
			wprintf(L"\tinline %s * ctor() { return this; }\n", nameFixed.c_str());
		}

		if (!hasDtor) {
			wprintf(L"\tinline void dtor() {}\n");
		}
	}

	// guard
	if (bGuardObject)
	{
		ULONGLONG len = 0;
		pUDT->get_length(&len);

		wprintf(L"\tinline void _guard_obj() {\n");
		wprintf(L"\t\tstatic_assert((sizeof(%s)==%u),\"bad size\");\n", nameFixed.c_str(), (unsigned int)len);

		SymbolEnumerator symbol;
		if (symbol.Find(pUDT, SymTagData, NULL)) {
			while (symbol.Next()) {

				DWORD dwDataKind;
				symbol->get_dataKind(&dwDataKind);
				if (dwDataKind == DataIsMember)
				{
					DWORD locType = 0;
					symbol->get_locationType(&locType);
					if (locType == LocIsThisRel)
					{
						LONG off = 0;
						if (symbol->get_offset(&off) == S_OK)
						{
							ComRef<IDiaSymbol> fieldType;
							symbol->get_type(&fieldType);
							BOOL isRef = FALSE;
							fieldType->get_reference(&isRef);

							if (!isRef) {
								Bstr fieldName;
								symbol->get_name(&fieldName);
								wprintf(L"\t\tstatic_assert((offsetof(%s,%s)==0x%X),\"bad off\");\n", nameFixed.c_str(), *fieldName, (unsigned int)off);
							}
						}
					}
				}
			}
		}
		wprintf(L"\t};\n");
	}

	wprintf(L"};\n"); // end UDT
	wprintf(L"\n");
}
