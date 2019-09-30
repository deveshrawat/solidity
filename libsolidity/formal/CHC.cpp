/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "libsolidity/formal/SolverInterface.h"
#include <libsolidity/formal/CHC.h>

#include <libsolidity/formal/CHCSmtLib2Interface.h>

#ifdef HAVE_Z3
#include <libsolidity/formal/Z3CHCInterface.h>
#endif

#include <libsolidity/formal/SymbolicTypes.h>

#include <libsolidity/ast/TypeProvider.h>

#include <queue>

using namespace std;
using namespace dev;
using namespace langutil;
using namespace dev::solidity;

CHC::CHC(
	smt::EncodingContext& _context,
	ErrorReporter& _errorReporter,
	map<h256, string> const& _smtlib2Responses,
	ReadCallback::Callback const& _smtCallback,
	[[maybe_unused]] smt::SMTSolverChoice _enabledSolvers
):
	SMTEncoder(_context),
	m_outerErrorReporter(_errorReporter),
	m_enabledSolvers(_enabledSolvers)
{
#ifdef HAVE_Z3
	if (_enabledSolvers.z3)
		m_interface = make_unique<smt::Z3CHCInterface>();
#endif
	if (!m_interface)
		m_interface = make_unique<smt::CHCSmtLib2Interface>(_smtlib2Responses, _smtCallback);
}

void CHC::analyze(SourceUnit const& _source)
{
	solAssert(_source.annotation().experimentalFeatures.count(ExperimentalFeature::SMTChecker), "");

	bool usesZ3 = false;
#ifdef HAVE_Z3
	usesZ3 = m_enabledSolvers.z3;
	if (usesZ3)
	{
		auto z3Interface = dynamic_cast<smt::Z3CHCInterface const*>(m_interface.get());
		solAssert(z3Interface, "");
		m_context.setSolver(z3Interface->z3Interface());
	}
#endif
	if (!usesZ3)
	{
		auto smtlib2Interface = dynamic_cast<smt::CHCSmtLib2Interface const*>(m_interface.get());
		solAssert(smtlib2Interface, "");
		m_context.setSolver(smtlib2Interface->smtlib2Interface());
	}
	m_context.clear();
	m_context.setAssertionAccumulation(false);
	m_variableUsage.setFunctionInlining(false);

	resetSourceAnalysis();

	auto boolSort = make_shared<smt::Sort>(smt::Kind::Bool);
	auto genesisSort = make_shared<smt::FunctionSort>(
		vector<smt::SortPointer>(),
		boolSort
	);
	m_genesisPredicate = createSymbolicBlock(genesisSort, "genesis");
	addRule(genesis(), "genesis");

	defineSummaries(_source);
	for (auto const& source: _source.referencedSourceUnits())
		defineSummaries(*source);

	_source.accept(*this);
	while (!m_toAnalyzeContracts.empty())
	{
		auto toAnalyze = move(m_toAnalyzeContracts);
		m_toAnalyzeContracts.clear();
		for (auto const* contract: toAnalyze)
			if (!m_analyzedContracts.count(contract))
				contract->accept(*this);
	}

	set<Expression const*> unsafeAssertions;
	for (auto const& [function, error]: m_functionErrors)
	{
		auto assertions = transactionAssertions(function);
		if (assertions.empty())
			continue;
		auto [result, model] = query(error, function->location());
		if (!result)
		{
			if (model.size() == 0)
				unsafeAssertions.insert(assertions.begin(), assertions.end());
			else
			{
				int assertion = stoi(model.at(0));
				unsafeAssertions.insert(m_verificationTargets.at(assertion - 1));
			}
		}
	}
	for (auto const& assertion: m_verificationTargets)
		if (!unsafeAssertions.count(assertion))
			m_safeAssertions.insert(assertion);
}

vector<string> CHC::unhandledQueries() const
{
	if (auto smtlib2 = dynamic_cast<smt::CHCSmtLib2Interface const*>(m_interface.get()))
		return smtlib2->unhandledQueries();

	return {};
}

bool CHC::visit(ContractDefinition const& _contract)
{
	solAssert(!m_analyzedContracts.count(&_contract), "");
	m_analyzedContracts.insert(&_contract);

	resetContractAnalysis();

	initContract(_contract);

	m_stateVariables = _contract.stateVariablesIncludingInherited();
	for (auto const& var: m_stateVariables)
		m_stateSorts.push_back(smt::smtSortAbstractFunction(*var->type()));

	clearIndices(&_contract);

	if (_contract.contractKind() == ContractDefinition::ContractKind::Contract)
	{
		string suffix = _contract.name() + "_" + to_string(_contract.id());
		m_interfacePredicate = createSymbolicBlock(interfaceSort(), "interface_" + suffix);

		// TODO create static instances for Bool/Int sorts in SolverInterface.
		auto boolSort = make_shared<smt::Sort>(smt::Kind::Bool);
		auto errorFunctionSort = make_shared<smt::FunctionSort>(
			vector<smt::SortPointer>(),
			boolSort
		);

		m_errorPredicate = createSymbolicBlock(errorFunctionSort, "error_" + suffix);
		m_constructorSummaryPredicate = createSymbolicBlock(constructorSort(), "summary_constructor_" + suffix);
		m_implicitConstructorPredicate = createSymbolicBlock(interfaceSort(), "implicit_constructor_" + suffix);
		auto stateExprs = currentStateVariables();
		setCurrentBlock(*m_interfacePredicate, &stateExprs);
	}

	SMTEncoder::visit(_contract);
	return false;
}

void CHC::endVisit(ContractDefinition const& _contract)
{
	if (_contract.contractKind() != ContractDefinition::ContractKind::Contract)
	{
		SMTEncoder::endVisit(_contract);
		return;
	}

	for (auto const& var: m_stateVariables)
	{
		solAssert(m_context.knownVariable(*var), "");
		auto const& symbVar = m_context.variable(*var);
		symbVar->resetIndex();
		m_context.setZeroValue(*var);
		symbVar->increaseIndex();
	}
	auto implicitConstructor = (*m_implicitConstructorPredicate)(initialStateVariables());
	connectBlocks(genesis(), implicitConstructor);
	m_currentBlock = implicitConstructor;
	m_context.addAssertion(m_error.currentValue() == 0);

	if (auto constructor = _contract.constructor())
		constructor->accept(*this);
	else
		inlineConstructorHierarchy(_contract);

	auto summary = predicate(*m_constructorSummaryPredicate, vector<smt::Expression>{m_error.currentValue()} + currentStateVariables());
	connectBlocks(m_currentBlock, summary);

	// TODO this is a hack, fix it
	m_context.pushSolver();
	clearIndices(m_currentContract, nullptr);
	auto stateExprs = vector<smt::Expression>{m_error.currentValue()} + currentStateVariables();
	setCurrentBlock(*m_constructorSummaryPredicate, &stateExprs);

	createErrorBlock();
	connectBlocks(m_currentBlock, error(), m_error.currentValue() > 0);
	m_functionErrors.emplace(m_currentContract, error());

	connectBlocks(m_currentBlock, interface(), m_error.currentValue() == 0);

	SMTEncoder::endVisit(_contract);
}

bool CHC::visit(FunctionDefinition const& _function)
{
	if (!shouldVisit(_function))
		return false;

	// This is the case for base constructor inlining.
	if (m_currentFunction)
	{
		solAssert(m_currentFunction->isConstructor(), "");
		solAssert(_function.isConstructor(), "");
		solAssert(_function.scope() != m_currentContract, "");
		SMTEncoder::visit(_function);
		return false;
	}

	solAssert(!m_currentFunction, "Function inlining should not happen in CHC.");
	m_currentFunction = &_function;

	initFunction(_function);

	auto functionEntryBlock = createBlock(m_currentFunction);
	auto bodyBlock = createBlock(&m_currentFunction->body());

	auto functionPred = predicate(*functionEntryBlock, currentFunctionVariables());
	auto bodyPred = predicate(*bodyBlock);

	if (_function.isConstructor())
		connectBlocks(m_currentBlock, functionPred);
	else
		connectBlocks(genesis(), functionPred);

	m_context.addAssertion(m_error.currentValue() == 0);
	for (auto const* var: m_stateVariables)
		m_context.addAssertion(m_context.variable(*var)->valueAtIndex(0) == currentValue(*var));
	for (auto const& var: _function.parameters())
		m_context.addAssertion(m_context.variable(*var)->valueAtIndex(0) == currentValue(*var));

	connectBlocks(functionPred, bodyPred);

	setCurrentBlock(*bodyBlock);

	SMTEncoder::visit(*m_currentFunction);

	return false;
}

void CHC::endVisit(FunctionDefinition const& _function)
{
	if (!shouldVisit(_function))
		return;

	// This is the case for base constructor inlining.
	if (m_currentFunction != &_function)
	{
		solAssert(m_currentFunction && m_currentFunction->isConstructor(), "");
		solAssert(_function.isConstructor(), "");
		solAssert(_function.scope() != m_currentContract, "");
	}
	else
	{
		// We create an extra exit block for constructors that simply
		// connects to the interface in case an explicit constructor
		// exists in the hierarchy.
		// It is not connected directly here, as normal functions are,
		// because of the case where there are only implicit constructors.
		// This is done in endVisit(ContractDefinition).
		if (_function.isConstructor())
		{
			string suffix = m_currentContract->name() + "_" + to_string(m_currentContract->id());
			auto constructorExit = createSymbolicBlock(constructorSort(), "constructor_exit_" + suffix);
			connectBlocks(m_currentBlock, predicate(*constructorExit, vector<smt::Expression>{m_error.currentValue()} + currentStateVariables()));

			clearIndices(m_currentContract, m_currentFunction);
			auto stateExprs = vector<smt::Expression>{m_error.currentValue()} + currentStateVariables();
			setCurrentBlock(*constructorExit, &stateExprs);
		}
		else
		{
			auto sum = summary(_function);
			connectBlocks(m_currentBlock, sum);

			if (m_currentContract->contractKind() == ContractDefinition::ContractKind::Contract)
			{
				auto iface = interface();

				auto stateExprs = initialStateVariables();
				setCurrentBlock(*m_interfacePredicate, &stateExprs);

				if (_function.isPublic())
				{
					createErrorBlock();
					connectBlocks(m_currentBlock, error(), sum && (m_error.currentValue() > 0));
					connectBlocks(m_currentBlock, iface, sum && (m_error.currentValue() == 0));
					m_functionErrors.emplace(&_function, error());
				}
			}
		}
		m_currentFunction = nullptr;
	}

	SMTEncoder::endVisit(_function);
}

bool CHC::visit(IfStatement const& _if)
{
	solAssert(m_currentFunction, "");

	bool unknownFunctionCallWasSeen = m_unknownFunctionCallSeen;
	m_unknownFunctionCallSeen = false;

	solAssert(m_currentFunction, "");
	auto const& functionBody = m_currentFunction->body();

	auto ifHeaderBlock = createBlock(&_if, "if_header_");
	auto trueBlock = createBlock(&_if.trueStatement(), "if_true_");
	auto falseBlock = _if.falseStatement() ? createBlock(_if.falseStatement(), "if_false_") : nullptr;
	auto afterIfBlock = createBlock(&functionBody);

	connectBlocks(m_currentBlock, predicate(*ifHeaderBlock));

	setCurrentBlock(*ifHeaderBlock);
	_if.condition().accept(*this);
	auto condition = expr(_if.condition());

	connectBlocks(m_currentBlock, predicate(*trueBlock), condition);
	if (_if.falseStatement())
		connectBlocks(m_currentBlock, predicate(*falseBlock), !condition);
	else
		connectBlocks(m_currentBlock, predicate(*afterIfBlock), !condition);

	setCurrentBlock(*trueBlock);
	_if.trueStatement().accept(*this);
	connectBlocks(m_currentBlock, predicate(*afterIfBlock));

	if (_if.falseStatement())
	{
		setCurrentBlock(*falseBlock);
		_if.falseStatement()->accept(*this);
		connectBlocks(m_currentBlock, predicate(*afterIfBlock));
	}

	setCurrentBlock(*afterIfBlock);

	if (m_unknownFunctionCallSeen)
		eraseKnowledge();

	m_unknownFunctionCallSeen = unknownFunctionCallWasSeen;

	return false;
}

bool CHC::visit(WhileStatement const& _while)
{
	bool unknownFunctionCallWasSeen = m_unknownFunctionCallSeen;
	m_unknownFunctionCallSeen = false;

	solAssert(m_currentFunction, "");
	auto const& functionBody = m_currentFunction->body();

	auto namePrefix = string(_while.isDoWhile() ? "do_" : "") + "while";
	auto loopHeaderBlock = createBlock(&_while, namePrefix + "_header_");
	auto loopBodyBlock = createBlock(&_while.body(), namePrefix + "_body_");
	auto afterLoopBlock = createBlock(&functionBody);

	auto outerBreakDest = m_breakDest;
	auto outerContinueDest = m_continueDest;
	m_breakDest = afterLoopBlock.get();
	m_continueDest = loopHeaderBlock.get();

	if (_while.isDoWhile())
		_while.body().accept(*this);

	connectBlocks(m_currentBlock, predicate(*loopHeaderBlock));

	setCurrentBlock(*loopHeaderBlock);

	_while.condition().accept(*this);
	auto condition = expr(_while.condition());

	connectBlocks(m_currentBlock, predicate(*loopBodyBlock), condition);
	connectBlocks(m_currentBlock, predicate(*afterLoopBlock), !condition);

	// Loop body visit.
	setCurrentBlock(*loopBodyBlock);
	_while.body().accept(*this);

	m_breakDest = outerBreakDest;
	m_continueDest = outerContinueDest;

	// Back edge.
	connectBlocks(m_currentBlock, predicate(*loopHeaderBlock));
	setCurrentBlock(*afterLoopBlock);

	if (m_unknownFunctionCallSeen)
		eraseKnowledge();

	m_unknownFunctionCallSeen = unknownFunctionCallWasSeen;

	return false;
}

bool CHC::visit(ForStatement const& _for)
{
	bool unknownFunctionCallWasSeen = m_unknownFunctionCallSeen;
	m_unknownFunctionCallSeen = false;

	solAssert(m_currentFunction, "");
	auto const& functionBody = m_currentFunction->body();

	auto loopHeaderBlock = createBlock(&_for, "for_header_");
	auto loopBodyBlock = createBlock(&_for.body(), "for_body_");
	auto afterLoopBlock = createBlock(&functionBody);
	auto postLoop = _for.loopExpression();
	auto postLoopBlock = postLoop ? createBlock(postLoop, "for_post_") : nullptr;

	auto outerBreakDest = m_breakDest;
	auto outerContinueDest = m_continueDest;
	m_breakDest = afterLoopBlock.get();
	m_continueDest = postLoop ? postLoopBlock.get() : loopHeaderBlock.get();

	if (auto init = _for.initializationExpression())
		init->accept(*this);

	connectBlocks(m_currentBlock, predicate(*loopHeaderBlock));
	setCurrentBlock(*loopHeaderBlock);

	auto condition = smt::Expression(true);
	if (auto forCondition = _for.condition())
	{
		forCondition->accept(*this);
		condition = expr(*forCondition);
	}

	connectBlocks(m_currentBlock, predicate(*loopBodyBlock), condition);
	connectBlocks(m_currentBlock, predicate(*afterLoopBlock), !condition);

	// Loop body visit.
	setCurrentBlock(*loopBodyBlock);
	_for.body().accept(*this);

	if (postLoop)
	{
		connectBlocks(m_currentBlock, predicate(*postLoopBlock));
		setCurrentBlock(*postLoopBlock);
		postLoop->accept(*this);
	}

	m_breakDest = outerBreakDest;
	m_continueDest = outerContinueDest;

	// Back edge.
	connectBlocks(m_currentBlock, predicate(*loopHeaderBlock));
	setCurrentBlock(*afterLoopBlock);

	if (m_unknownFunctionCallSeen)
		eraseKnowledge();

	m_unknownFunctionCallSeen = unknownFunctionCallWasSeen;

	return false;
}

void CHC::endVisit(FunctionCall const& _funCall)
{
	solAssert(_funCall.annotation().kind != FunctionCallKind::Unset, "");

	if (_funCall.annotation().kind != FunctionCallKind::FunctionCall)
	{
		SMTEncoder::endVisit(_funCall);
		return;
	}

	FunctionType const& funType = dynamic_cast<FunctionType const&>(*_funCall.expression().annotation().type);
	switch (funType.kind())
	{
	case FunctionType::Kind::Assert:
		visitAssert(_funCall);
		SMTEncoder::endVisit(_funCall);
		break;
	case FunctionType::Kind::Internal:
		internalFunctionCall(_funCall);
		break;
	case FunctionType::Kind::External:
	case FunctionType::Kind::DelegateCall:
	case FunctionType::Kind::BareCall:
	case FunctionType::Kind::BareCallCode:
	case FunctionType::Kind::BareDelegateCall:
	case FunctionType::Kind::BareStaticCall:
	case FunctionType::Kind::Creation:
	case FunctionType::Kind::KECCAK256:
	case FunctionType::Kind::ECRecover:
	case FunctionType::Kind::SHA256:
	case FunctionType::Kind::RIPEMD160:
	case FunctionType::Kind::BlockHash:
	case FunctionType::Kind::AddMod:
	case FunctionType::Kind::MulMod:
		SMTEncoder::endVisit(_funCall);
		unknownFunctionCall(_funCall);
		break;
	default:
		SMTEncoder::endVisit(_funCall);
		break;
	}

	createReturnedExpressions(_funCall);
}

void CHC::endVisit(Break const& _break)
{
	solAssert(m_breakDest, "");
	connectBlocks(m_currentBlock, predicate(*m_breakDest));
	auto breakGhost = createBlock(&_break, "break_ghost_");
	m_currentBlock = predicate(*breakGhost);
}

void CHC::endVisit(Continue const& _continue)
{
	solAssert(m_continueDest, "");
	connectBlocks(m_currentBlock, predicate(*m_continueDest));
	auto continueGhost = createBlock(&_continue, "continue_ghost_");
	m_currentBlock = predicate(*continueGhost);
}

void CHC::visitAssert(FunctionCall const& _funCall)
{
	auto const& args = _funCall.arguments();
	solAssert(args.size() == 1, "");
	solAssert(args.front()->annotation().type->category() == Type::Category::Bool, "");

	m_verificationTargets.push_back(&_funCall);
	solAssert(m_currentContract, "");
	solAssert(m_currentFunction, "");
	if (m_currentFunction->isConstructor())
		m_functionAssertions[m_currentContract].insert(&_funCall);
	else
		m_functionAssertions[m_currentFunction].insert(&_funCall);

	auto previousError = m_error.currentValue();
	m_error.increaseIndex();

	connectBlocks(
		m_currentBlock,
		m_currentFunction->isConstructor() ? summary(*m_currentContract) : summary(*m_currentFunction),
		currentPathConditions() && !m_context.expression(*args.front())->currentValue() && (m_error.currentValue() == m_verificationTargets.size())
	);

	m_context.addAssertion(m_context.expression(*args.front())->currentValue());
	m_context.addAssertion(m_error.currentValue() == previousError);

	auto assertEntry = createBlock(&_funCall);
	connectBlocks(m_currentBlock, predicate(*assertEntry));
	setCurrentBlock(*assertEntry);
}

void CHC::internalFunctionCall(FunctionCall const& _funCall)
{
	solAssert(m_currentContract, "");

	auto const* function = functionCallToDefinition(_funCall);
	if (function)
	{
		if (!m_currentFunction)
			m_callGraph[m_currentContract].insert(function);
		else
			m_callGraph[m_currentFunction].insert(function);
		auto const* contract = dynamic_cast<ContractDefinition const*>(function->scope());
		solAssert(contract, "");
		if (contract != m_currentContract && !m_toAnalyzeContracts.count(contract))
			m_toAnalyzeContracts.insert(contract);
	}

	auto previousError = m_error.currentValue();

	m_context.addAssertion(predicate(_funCall));

	connectBlocks(
		m_currentBlock,
		m_currentFunction ? summary(*m_currentFunction) : summary(*m_currentContract),
		(m_error.currentValue() > 0)
	);
	m_context.addAssertion(m_error.currentValue() == 0);
	m_error.increaseIndex();
	m_context.addAssertion(m_error.currentValue() == previousError);
}

void CHC::unknownFunctionCall(FunctionCall const&)
{
	/// Function calls are not handled at the moment,
	/// so always erase knowledge.
	/// TODO remove when function calls get predicates/blocks.
	eraseKnowledge();

	/// Used to erase outer scope knowledge in loops and ifs.
	/// TODO remove when function calls get predicates/blocks.
	m_unknownFunctionCallSeen = true;
}

void CHC::resetSourceAnalysis()
{
	m_verificationTargets.clear();
	m_safeAssertions.clear();
	m_functionErrors.clear();
	m_functionAssertions.clear();
	m_callGraph.clear();
	m_summaries.clear();
	m_analyzedContracts.clear();
	m_toAnalyzeContracts.clear();
}

void CHC::resetContractAnalysis()
{
	m_stateSorts.clear();
	m_stateVariables.clear();
	m_unknownFunctionCallSeen = false;
	m_breakDest = nullptr;
	m_continueDest = nullptr;
	m_error.resetIndex();
}

void CHC::eraseKnowledge()
{
	resetStateVariables();
	m_context.resetVariables([&](VariableDeclaration const& _variable) { return _variable.hasReferenceOrMappingType(); });
}

void CHC::clearIndices(ContractDefinition const* _contract, FunctionDefinition const* _function)
{
	SMTEncoder::clearIndices(_contract, _function);
	for (auto const* var: m_stateVariables)
		/// SSA index 0 is reserved for state variables at the beginning
		/// of the current transaction.
		m_context.variable(*var)->increaseIndex();
	if (_function)
	{
		for (auto const& var: _function->parameters() + _function->returnParameters())
			m_context.variable(*var)->increaseIndex();
		for (auto const& var: _function->localVariables())
			m_context.variable(*var)->increaseIndex();
	}

	m_error.resetIndex();
}
	
bool CHC::shouldVisit(FunctionDefinition const& _function) const
{
	return _function.isImplemented();
}

void CHC::setCurrentBlock(
	smt::SymbolicFunctionVariable const& _block,
	vector<smt::Expression> const* _arguments
)
{
	m_context.popSolver();
	solAssert(m_currentContract, "");
	clearIndices(m_currentContract, m_currentFunction);
	m_context.pushSolver();
	if (_arguments)
		m_currentBlock = predicate(_block, *_arguments);
	else
		m_currentBlock = predicate(_block);
}

set<Expression const*> CHC::transactionAssertions(ASTNode const* _txRoot)
{
	set<Expression const*> assertions;
	set<ASTNode const*> visited;
	queue<ASTNode const*> toVisit;
	toVisit.push(_txRoot);
	while (!toVisit.empty())
	{
		auto const* function = toVisit.front();
		toVisit.pop();
		visited.insert(function);
		assertions.insert(m_functionAssertions[function].begin(), m_functionAssertions[function].end());
		for (auto const* called: m_callGraph[function])
			if (!visited.count(called))
				toVisit.push(called);
	}
	return assertions;
}

smt::SortPointer CHC::constructorSort()
{
	auto boolSort = make_shared<smt::Sort>(smt::Kind::Bool);
	auto intSort = make_shared<smt::Sort>(smt::Kind::Int);
	return make_shared<smt::FunctionSort>(
		vector<smt::SortPointer>{intSort} + m_stateSorts,
		boolSort
	);
}

smt::SortPointer CHC::interfaceSort()
{
	auto boolSort = make_shared<smt::Sort>(smt::Kind::Bool);
	return make_shared<smt::FunctionSort>(
		m_stateSorts,
		boolSort
	);
}

/// A function in the symbolic CFG requires:
/// - Index of failed assertion. 0 means no assertion failed.
/// - 2 sets of state variables:
///   - State variables at the beginning of the current function, immutable
///   - Current state variables
///    At the beginning of the function these must equal set 1
/// - 2 sets of input variables:
///   - Input variables at the beginning of the current function, immutable
///   - Current input variables
///    At the beginning of the function these must equal set 1
/// - 1 set of output variables
smt::SortPointer CHC::sort(FunctionDefinition const& _function)
{
	auto boolSort = make_shared<smt::Sort>(smt::Kind::Bool);
	auto intSort = make_shared<smt::Sort>(smt::Kind::Int);
	vector<smt::SortPointer> inputSorts;
	for (auto const& var: _function.parameters())
		inputSorts.push_back(smt::smtSortAbstractFunction(*var->type()));
	vector<smt::SortPointer> outputSorts;
	for (auto const& var: _function.returnParameters())
		outputSorts.push_back(smt::smtSortAbstractFunction(*var->type()));
	return make_shared<smt::FunctionSort>(
		vector<smt::SortPointer>{intSort} + m_stateSorts + inputSorts + m_stateSorts + inputSorts + outputSorts,
		boolSort
	);
}

smt::SortPointer CHC::sort(ASTNode const* _node)
{
	if (auto funDef = dynamic_cast<FunctionDefinition const*>(_node))
		return sort(*funDef);

	auto fSort = dynamic_pointer_cast<smt::FunctionSort>(sort(*m_currentFunction));
	solAssert(fSort, "");

	auto boolSort = make_shared<smt::Sort>(smt::Kind::Bool);
	vector<smt::SortPointer> varSorts;
	for (auto const& var: m_currentFunction->localVariables())
		varSorts.push_back(smt::smtSortAbstractFunction(*var->type()));
	return make_shared<smt::FunctionSort>(
		fSort->domain + varSorts,
		boolSort
	);
}

smt::SortPointer CHC::summarySort(FunctionDefinition const& _function, ContractDefinition const& _contract)
{
	auto stateVariables = _contract.stateVariablesIncludingInherited();
	std::vector<smt::SortPointer> stateSorts;
	for (auto const& var: stateVariables)
		stateSorts.push_back(smt::smtSortAbstractFunction(*var->type()));

	auto boolSort = make_shared<smt::Sort>(smt::Kind::Bool);
	auto intSort = make_shared<smt::Sort>(smt::Kind::Int);
	vector<smt::SortPointer> inputSorts, outputSorts;
	for (auto const& var: _function.parameters())
		inputSorts.push_back(smt::smtSortAbstractFunction(*var->type()));
	for (auto const& var: _function.returnParameters())
		outputSorts.push_back(smt::smtSortAbstractFunction(*var->type()));
	return make_shared<smt::FunctionSort>(
		vector<smt::SortPointer>{intSort} + stateSorts + inputSorts + stateSorts + outputSorts,
		boolSort
	);
}

unique_ptr<smt::SymbolicFunctionVariable> CHC::createSymbolicBlock(smt::SortPointer _sort, string const& _name)
{
	auto block = make_unique<smt::SymbolicFunctionVariable>(
		_sort,
		_name,
		m_context
	);
	m_interface->registerRelation(block->currentFunctionValue());
	return block;
}

void CHC::defineSummaries(SourceUnit const& _source)
{
	for (auto const& node : _source.nodes())
		if (auto const* contract = dynamic_cast<ContractDefinition const*>(node.get()))
			for (auto const* function: overrideResolvedFunctions(*contract))
				m_summaries[contract].emplace(function, createSummaryBlock(*function, *contract));
}

smt::Expression CHC::interface()
{
	vector<smt::Expression> paramExprs;
	for (auto const& var: m_stateVariables)
		paramExprs.push_back(m_context.variable(*var)->currentValue());
	return (*m_interfacePredicate)(paramExprs);
}

smt::Expression CHC::error()
{
	return (*m_errorPredicate)({});
}

smt::Expression CHC::error(unsigned _idx)
{
	return m_errorPredicate->functionValueAtIndex(_idx)({});
}

smt::Expression CHC::summary(ContractDefinition const&)
{
	return (*m_constructorSummaryPredicate)(
		vector<smt::Expression>{m_error.currentValue()} +
		currentStateVariables()
	);
}

smt::Expression CHC::summary(FunctionDefinition const& _function)
{
	vector<smt::Expression> args{m_error.currentValue()};
	args += initialStateVariables();
	for (auto const& var: _function.parameters())
		args.push_back(m_context.variable(*var)->valueAtIndex(0));
	args += currentStateVariables();
	for (auto const& var: _function.returnParameters())
		args.push_back(m_context.variable(*var)->currentValue());
	return (*m_summaries.at(m_currentContract).at(&_function))(args);
}

unique_ptr<smt::SymbolicFunctionVariable> CHC::createBlock(ASTNode const* _node, string const& _prefix)
{
	return createSymbolicBlock(sort(_node),
		"block_" +
		uniquePrefix() +
		"_" +
		_prefix +
		predicateName(_node));
}

unique_ptr<smt::SymbolicFunctionVariable> CHC::createSummaryBlock(FunctionDefinition const& _function, ContractDefinition const& _contract)
{
	return createSymbolicBlock(summarySort(_function, _contract),
		"summary_" +
		uniquePrefix() +
		"_" +
		predicateName(&_function, &_contract));
}

void CHC::createErrorBlock()
{
	solAssert(m_errorPredicate, "");
	m_errorPredicate->increaseIndex();
	m_interface->registerRelation(m_errorPredicate->currentFunctionValue());
}

void CHC::connectBlocks(smt::Expression const& _from, smt::Expression const& _to, smt::Expression const& _constraints)
{
	smt::Expression edge = smt::Expression::implies(
		_from && m_context.assertions() && _constraints,
		_to
	);
	addRule(edge, _from.name + "_to_" + _to.name);
}

vector<smt::Expression> CHC::initialStateVariables()
{
	return stateVariablesAtIndex(0);
}

vector<smt::Expression> CHC::stateVariablesAtIndex(int _index)
{
	solAssert(m_currentContract, "");
	vector<smt::Expression> exprs;
	for (auto const& var: m_stateVariables)
		exprs.push_back(m_context.variable(*var)->valueAtIndex(_index));
	return exprs;
}

vector<smt::Expression> CHC::currentStateVariables()
{
	solAssert(m_currentContract, "");
	vector<smt::Expression> exprs;
	for (auto const& var: m_stateVariables)
		exprs.push_back(m_context.variable(*var)->currentValue());
	return exprs;
}

vector<smt::Expression> CHC::currentFunctionVariables()
{
	vector<smt::Expression> initInputExprs;
	vector<smt::Expression> mutableInputExprs;
	for (auto const& var: m_currentFunction->parameters())
	{
		initInputExprs.push_back(m_context.variable(*var)->valueAtIndex(0));
		mutableInputExprs.push_back(m_context.variable(*var)->currentValue());
	}
	vector<smt::Expression> returnExprs;
	for (auto const& var: m_currentFunction->returnParameters())
		returnExprs.push_back(m_context.variable(*var)->currentValue());
	return vector<smt::Expression>{m_error.currentValue()} +
		initialStateVariables() +
		initInputExprs +
		currentStateVariables() +
		mutableInputExprs +
		returnExprs;
}

vector<smt::Expression> CHC::currentBlockVariables()
{
	vector<smt::Expression> paramExprs;
	if (m_currentFunction)
		for (auto const& var: m_currentFunction->localVariables())
			paramExprs.push_back(m_context.variable(*var)->currentValue());
	return currentFunctionVariables() + paramExprs;
}

string CHC::predicateName(ASTNode const* _node, ContractDefinition const* _contract)
{
	string prefix;
	if (auto funDef = dynamic_cast<FunctionDefinition const*>(_node))
		prefix += TokenTraits::toString(funDef->kind()) + (funDef->name().empty() ? "" : (string("_") + funDef->name()));
	else if (m_currentFunction && !m_currentFunction->name().empty())
		prefix += m_currentFunction->name();

	auto contract = _contract ? _contract : m_currentContract;
	solAssert(contract, "");
	return prefix + "_" + to_string(_node->id()) + "_" + to_string(contract->id());
}

smt::Expression CHC::predicate(smt::SymbolicFunctionVariable const& _block)
{
	return _block(currentBlockVariables());
}

smt::Expression CHC::predicate(
	smt::SymbolicFunctionVariable const& _block,
	vector<smt::Expression> const& _arguments
)
{
	return _block(_arguments);
}

smt::Expression CHC::predicate(FunctionCall const& _funCall)
{
	auto const* function = functionCallToDefinition(_funCall);
	if (!function)
		return smt::Expression(true);

	m_error.increaseIndex();
	vector<smt::Expression> args{m_error.currentValue()};
	args += currentStateVariables();
	args += symbolicArguments(_funCall);
	for (auto const& var: m_stateVariables)
		m_context.variable(*var)->increaseIndex();
	args += currentStateVariables();

	auto const& returnParams = function->returnParameters();
	for (auto param: returnParams)
		if (m_context.knownVariable(*param))
			m_context.variable(*param)->increaseIndex();
		else
			createVariable(*param);
	for (auto const& var: function->returnParameters())
		args.push_back(m_context.variable(*var)->currentValue());
	auto const* contract = dynamic_cast<ContractDefinition const*>(function->scope());
	return (*m_summaries.at(contract).at(function))(args);
}

void CHC::addRule(smt::Expression const& _rule, string const& _ruleName)
{
	m_interface->addRule(_rule, _ruleName);
}

pair<bool, vector<string>> CHC::query(smt::Expression const& _query, langutil::SourceLocation const& _location)
{
	smt::CheckResult result;
	vector<string> values;
	tie(result, values) = m_interface->query(_query);
	switch (result)
	{
	case smt::CheckResult::SATISFIABLE:
		break;
	case smt::CheckResult::UNSATISFIABLE:
		return {true, values};
	case smt::CheckResult::UNKNOWN:
		break;
	case smt::CheckResult::CONFLICTING:
		m_outerErrorReporter.warning(_location, "At least two SMT solvers provided conflicting answers. Results might not be sound.");
		break;
	case smt::CheckResult::ERROR:
		m_outerErrorReporter.warning(_location, "Error trying to invoke SMT solver.");
		break;
	}
	return {false, values};
}

string CHC::uniquePrefix()
{
	return to_string(m_blockCounter++);
}
