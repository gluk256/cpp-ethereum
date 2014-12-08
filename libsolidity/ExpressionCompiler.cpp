/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * @author Christian <c@ethdev.com>
 * @date 2014
 * Solidity AST to EVM bytecode compiler for expressions.
 */

#include <utility>
#include <numeric>
#include <libdevcore/Common.h>
#include <libsolidity/AST.h>
#include <libsolidity/ExpressionCompiler.h>
#include <libsolidity/CompilerContext.h>

using namespace std;

namespace dev {
namespace solidity {

void ExpressionCompiler::compileExpression(CompilerContext& _context, Expression& _expression)
{
	ExpressionCompiler compiler(_context);
	_expression.accept(compiler);
}

void ExpressionCompiler::appendTypeConversion(CompilerContext& _context,
											  Type const& _typeOnStack, Type const& _targetType)
{
	ExpressionCompiler compiler(_context);
	compiler.appendTypeConversion(_typeOnStack, _targetType);
}

bool ExpressionCompiler::visit(Assignment& _assignment)
{
	_assignment.getRightHandSide().accept(*this);
	appendTypeConversion(*_assignment.getRightHandSide().getType(), *_assignment.getType());
	_assignment.getLeftHandSide().accept(*this);
	if (asserts(m_currentLValue.isValid()))
		BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("LValue not retrieved."));

	Token::Value op = _assignment.getAssignmentOperator();
	if (op != Token::ASSIGN) // compound assignment
	{
		if (m_currentLValue.storesReferenceOnStack())
			m_context << eth::Instruction::SWAP1 << eth::Instruction::DUP2;
		m_currentLValue.retrieveValue(_assignment, true);
		appendOrdinaryBinaryOperatorCode(Token::AssignmentToBinaryOp(op), *_assignment.getType());
	}
	m_currentLValue.storeValue(_assignment);
	m_currentLValue.reset();

	return false;
}

void ExpressionCompiler::endVisit(UnaryOperation& _unaryOperation)
{
	//@todo type checking and creating code for an operator should be in the same place:
	// the operator should know how to convert itself and to which types it applies, so
	// put this code together with "Type::acceptsBinary/UnaryOperator" into a class that
	// represents the operator
	switch (_unaryOperation.getOperator())
	{
	case Token::NOT: // !
		m_context << eth::Instruction::ISZERO;
		break;
	case Token::BIT_NOT: // ~
		m_context << eth::Instruction::NOT;
		break;
	case Token::DELETE: // delete
		// @todo semantics change for complex types
		if (asserts(m_currentLValue.isValid()))
			BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("LValue not retrieved."));

		m_context << u256(0);
		if (m_currentLValue.storesReferenceOnStack())
			m_context << eth::Instruction::SWAP1;
		m_currentLValue.storeValue(_unaryOperation);
		m_currentLValue.reset();
		break;
	case Token::INC: // ++ (pre- or postfix)
	case Token::DEC: // -- (pre- or postfix)
		if (asserts(m_currentLValue.isValid()))
			BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("LValue not retrieved."));
		m_currentLValue.retrieveValue(_unaryOperation);
		if (!_unaryOperation.isPrefixOperation())
		{
			if (m_currentLValue.storesReferenceOnStack())
				m_context << eth::Instruction::SWAP1 << eth::Instruction::DUP2;
			else
				m_context << eth::Instruction::DUP1;
		}
		m_context << u256(1);
		if (_unaryOperation.getOperator() == Token::INC)
			m_context << eth::Instruction::ADD;
		else
			m_context << eth::Instruction::SWAP1 << eth::Instruction::SUB; // @todo avoid the swap
		// Stack for prefix: [ref] (*ref)+-1
		// Stack for postfix: *ref [ref] (*ref)+-1
		if (m_currentLValue.storesReferenceOnStack())
			m_context << eth::Instruction::SWAP1;
		m_currentLValue.storeValue(_unaryOperation, !_unaryOperation.isPrefixOperation());
		m_currentLValue.reset();
		break;
	case Token::ADD: // +
		// unary add, so basically no-op
		break;
	case Token::SUB: // -
		m_context << u256(0) << eth::Instruction::SUB;
		break;
	default:
		BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Invalid unary operator: " +
																		 string(Token::toString(_unaryOperation.getOperator()))));
	}
}

bool ExpressionCompiler::visit(BinaryOperation& _binaryOperation)
{
	Expression& leftExpression = _binaryOperation.getLeftExpression();
	Expression& rightExpression = _binaryOperation.getRightExpression();
	Type const& commonType = _binaryOperation.getCommonType();
	Token::Value const op = _binaryOperation.getOperator();

	if (op == Token::AND || op == Token::OR) // special case: short-circuiting
		appendAndOrOperatorCode(_binaryOperation);
	else
	{
		bool cleanupNeeded = false;
		if (commonType.getCategory() == Type::Category::INTEGER)
			if (Token::isCompareOp(op) || op == Token::DIV || op == Token::MOD)
				cleanupNeeded = true;

		rightExpression.accept(*this);
		appendTypeConversion(*rightExpression.getType(), commonType, cleanupNeeded);
		leftExpression.accept(*this);
		appendTypeConversion(*leftExpression.getType(), commonType, cleanupNeeded);
		if (Token::isCompareOp(op))
			appendCompareOperatorCode(op, commonType);
		else
			appendOrdinaryBinaryOperatorCode(op, commonType);
	}

	// do not visit the child nodes, we already did that explicitly
	return false;
}

bool ExpressionCompiler::visit(FunctionCall& _functionCall)
{
	using Location = FunctionType::Location;
	if (_functionCall.isTypeConversion())
	{
		//@todo struct construction
		if (asserts(_functionCall.getArguments().size() == 1))
			BOOST_THROW_EXCEPTION(InternalCompilerError());
		Expression& firstArgument = *_functionCall.getArguments().front();
		firstArgument.accept(*this);
		if (firstArgument.getType()->getCategory() == Type::Category::CONTRACT &&
				_functionCall.getType()->getCategory() == Type::Category::INTEGER)
		{
			// explicit type conversion contract -> address, nothing to do.
		}
		else
		{
			appendTypeConversion(*firstArgument.getType(), *_functionCall.getType());
		}
	}
	else
	{
		FunctionType const& function = dynamic_cast<FunctionType const&>(*_functionCall.getExpression().getType());
		std::vector<ASTPointer<Expression>> const& arguments = _functionCall.getArguments();
		if (asserts(arguments.size() == function.getParameterTypes().size()))
			BOOST_THROW_EXCEPTION(InternalCompilerError());

		switch (function.getLocation())
		{
		case Location::INTERNAL:
		{
			// Calling convention: Caller pushes return address and arguments
			// Callee removes them and pushes return values

			eth::AssemblyItem returnLabel = m_context.pushNewTag();
			for (unsigned i = 0; i < arguments.size(); ++i)
			{
				arguments[i]->accept(*this);
				appendTypeConversion(*arguments[i]->getType(), *function.getParameterTypes()[i]);
			}
			_functionCall.getExpression().accept(*this);

			m_context.appendJump();
			m_context << returnLabel;

			// callee adds return parameters, but removes arguments and return label
			m_context.adjustStackOffset(function.getReturnParameterTypes().size() - arguments.size() - 1);

			// @todo for now, the return value of a function is its first return value, so remove
			// all others
			for (unsigned i = 1; i < function.getReturnParameterTypes().size(); ++i)
				m_context << eth::Instruction::POP;
			break;
		}
		case Location::EXTERNAL:
		{
			unsigned dataOffset = 1; // reserve one byte for the function index
			for (unsigned i = 0; i < arguments.size(); ++i)
			{
				arguments[i]->accept(*this);
				Type const& type = *function.getParameterTypes()[i];
				appendTypeConversion(*arguments[i]->getType(), type);
				unsigned const numBytes = type.getCalldataEncodedSize();
				if (numBytes == 0 || numBytes > 32)
					BOOST_THROW_EXCEPTION(CompilerError()
										  << errinfo_sourceLocation(arguments[i]->getLocation())
										  << errinfo_comment("Type " + type.toString() + " not yet supported."));
				if (numBytes != 32)
					m_context << (u256(1) << ((32 - numBytes) * 8)) << eth::Instruction::MUL;
				m_context << u256(dataOffset) << eth::Instruction::MSTORE;
				dataOffset += numBytes;
			}
			//@todo only return the first return value for now
			unsigned retSize = function.getReturnParameterTypes().empty() ? 0
									: function.getReturnParameterTypes().front()->getCalldataEncodedSize();
			// CALL arguments: outSize, outOff, inSize, inOff, value, addr, gas (stack top)
			m_context << u256(retSize) << u256(0) << u256(dataOffset) << u256(0) << u256(0);
			_functionCall.getExpression().accept(*this); // pushes addr and function index
			m_context << u256(0) << eth::Instruction::MSTORE8
					  << u256(25) << eth::Instruction::GAS << eth::Instruction::SUB
					  << eth::Instruction::CALL
					  << eth::Instruction::POP; // @todo do not ignore failure indicator
			if (retSize == 32)
				m_context << u256(0) << eth::Instruction::MLOAD;
			else if (retSize > 0)
				m_context << (u256(1) << ((32 - retSize) * 8))
						  << u256(0) << eth::Instruction::MLOAD << eth::Instruction::DIV;
			break;
		}
		case Location::SEND:
			m_context << u256(0) << u256(0) << u256(0) << u256(0);
			arguments.front()->accept(*this);
			//@todo might not be necessary
			appendTypeConversion(*arguments.front()->getType(), *function.getParameterTypes().front(), true);
			_functionCall.getExpression().accept(*this);
			m_context << u256(25) << eth::Instruction::GAS << eth::Instruction::SUB
					  << eth::Instruction::CALL
					  << eth::Instruction::POP;
			break;
		case Location::SUICIDE:
			arguments.front()->accept(*this);
			//@todo might not be necessary
			appendTypeConversion(*arguments.front()->getType(), *function.getParameterTypes().front(), true);
			m_context << eth::Instruction::SUICIDE;
			break;
		case Location::SHA3:
			arguments.front()->accept(*this);
			appendTypeConversion(*arguments.front()->getType(), *function.getParameterTypes().front(), true);
			// @todo move this once we actually use memory
			m_context << u256(0) << eth::Instruction::MSTORE << u256(32) << u256(0) << eth::Instruction::SHA3;
			break;
		case Location::ECRECOVER:
		case Location::SHA256:
		case Location::RIPEMD160:
		{
			static const map<Location, u256> contractAddresses{{Location::ECRECOVER, 1},
															   {Location::SHA256, 2},
															   {Location::RIPEMD160, 3}};
			u256 contractAddress = contractAddresses.find(function.getLocation())->second;
			// @todo later, combine this code with external function call
			for (unsigned i = 0; i < arguments.size(); ++i)
			{
				arguments[i]->accept(*this);
				appendTypeConversion(*arguments[i]->getType(), *function.getParameterTypes()[i], true);
				// @todo move this once we actually use memory
				m_context << u256(i * 32) << eth::Instruction::MSTORE;
			}
			m_context << u256(32) << u256(0) << u256(arguments.size() * 32) << u256(0) << u256(0)
					  << contractAddress << u256(500) //@todo determine actual gas requirement
					  << eth::Instruction::CALL
					  << eth::Instruction::POP
					  << u256(0) << eth::Instruction::MLOAD;
			break;
		}
		default:
			BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Invalid function type."));
		}
	}
	return false;
}

void ExpressionCompiler::endVisit(MemberAccess& _memberAccess)
{
	ASTString const& member = _memberAccess.getMemberName();
	switch (_memberAccess.getExpression().getType()->getCategory())
	{
	case Type::Category::INTEGER:
		if (member == "balance")
		{
			appendTypeConversion(*_memberAccess.getExpression().getType(),
								 IntegerType(0, IntegerType::Modifier::ADDRESS), true);
			m_context << eth::Instruction::BALANCE;
		}
		else if (member == "send")
		{
			appendTypeConversion(*_memberAccess.getExpression().getType(),
								 IntegerType(0, IntegerType::Modifier::ADDRESS), true);
		}
		else
			BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Invalid member access to integer."));
		break;
	case Type::Category::CONTRACT:
	{
		ContractType const& type = dynamic_cast<ContractType const&>(*_memberAccess.getExpression().getType());
		m_context << type.getFunctionIndex(member);
		break;
	}
	case Type::Category::MAGIC:
		// we can ignore the kind of magic and only look at the name of the member
		if (member == "coinbase")
			m_context << eth::Instruction::COINBASE;
		else if (member == "timestamp")
			m_context << eth::Instruction::TIMESTAMP;
		else if (member == "prevhash")
			m_context << eth::Instruction::PREVHASH;
		else if (member == "difficulty")
			m_context << eth::Instruction::DIFFICULTY;
		else if (member == "number")
			m_context << eth::Instruction::NUMBER;
		else if (member == "gaslimit")
			m_context << eth::Instruction::GASLIMIT;
		else if (member == "sender")
			m_context << eth::Instruction::CALLER;
		else if (member == "value")
			m_context << eth::Instruction::CALLVALUE;
		else if (member == "origin")
			m_context << eth::Instruction::ORIGIN;
		else if (member == "gas")
			m_context << eth::Instruction::GAS;
		else if (member == "gasprice")
			m_context << eth::Instruction::GASPRICE;
		else
			BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Unknown magic member."));
		break;
	case Type::Category::STRUCT:
	{
		StructType const& type = dynamic_cast<StructType const&>(*_memberAccess.getExpression().getType());
		m_context << type.getStorageOffsetOfMember(member) << eth::Instruction::ADD;
		m_currentLValue = LValue(m_context, LValue::STORAGE);
		m_currentLValue.retrieveValueIfLValueNotRequested(_memberAccess);
		break;
	}
	default:
		BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Member access to unknown type."));
	}
}

bool ExpressionCompiler::visit(IndexAccess& _indexAccess)
{
	_indexAccess.getBaseExpression().accept(*this);
	_indexAccess.getIndexExpression().accept(*this);
	appendTypeConversion(*_indexAccess.getIndexExpression().getType(),
						 *dynamic_cast<MappingType const&>(*_indexAccess.getBaseExpression().getType()).getKeyType(),
						 true);
	// @todo move this once we actually use memory
	m_context << u256(32) << eth::Instruction::MSTORE << u256(0) << eth::Instruction::MSTORE;
	m_context << u256(64) << u256(0) << eth::Instruction::SHA3;

	m_currentLValue = LValue(m_context, LValue::STORAGE);
	m_currentLValue.retrieveValueIfLValueNotRequested(_indexAccess);

	return false;
}

void ExpressionCompiler::endVisit(Identifier& _identifier)
{
	Declaration* declaration = _identifier.getReferencedDeclaration();
	if (MagicVariableDeclaration* magicVar = dynamic_cast<MagicVariableDeclaration*>(declaration))
	{
		if (magicVar->getType()->getCategory() == Type::Category::CONTRACT) // must be "this"
			m_context << eth::Instruction::ADDRESS;
		return;
	}
	if (FunctionDefinition* functionDef = dynamic_cast<FunctionDefinition*>(declaration))
	{
		m_context << m_context.getFunctionEntryLabel(*functionDef).pushTag();
		return;
	}
	if (/*VariableDeclaration* varDef = */dynamic_cast<VariableDeclaration*>(declaration))
	{
		m_currentLValue.fromIdentifier(_identifier, *_identifier.getReferencedDeclaration());
		m_currentLValue.retrieveValueIfLValueNotRequested(_identifier);
		return;
	}
	BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Identifier type not expected in expression context."));
}

void ExpressionCompiler::endVisit(Literal& _literal)
{
	switch (_literal.getType()->getCategory())
	{
	case Type::Category::INTEGER:
	case Type::Category::BOOL:
		m_context << _literal.getType()->literalValue(_literal);
		break;
	default:
		BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Only integer and boolean literals implemented for now."));
	}
}

void ExpressionCompiler::appendAndOrOperatorCode(BinaryOperation& _binaryOperation)
{
	Token::Value const op = _binaryOperation.getOperator();
	if (asserts(op == Token::OR || op == Token::AND))
		BOOST_THROW_EXCEPTION(InternalCompilerError());

	_binaryOperation.getLeftExpression().accept(*this);
	m_context << eth::Instruction::DUP1;
	if (op == Token::AND)
		m_context << eth::Instruction::ISZERO;
	eth::AssemblyItem endLabel = m_context.appendConditionalJump();
	m_context << eth::Instruction::POP;
	_binaryOperation.getRightExpression().accept(*this);
	m_context << endLabel;
}

void ExpressionCompiler::appendCompareOperatorCode(Token::Value _operator, Type const& _type)
{
	if (_operator == Token::EQ || _operator == Token::NE)
	{
		m_context << eth::Instruction::EQ;
		if (_operator == Token::NE)
			m_context << eth::Instruction::ISZERO;
	}
	else
	{
		IntegerType const& type = dynamic_cast<IntegerType const&>(_type);
		bool const isSigned = type.isSigned();

		switch (_operator)
		{
		case Token::GTE:
			m_context << (isSigned ? eth::Instruction::SLT : eth::Instruction::LT)
					  << eth::Instruction::ISZERO;
			break;
		case Token::LTE:
			m_context << (isSigned ? eth::Instruction::SGT : eth::Instruction::GT)
					  << eth::Instruction::ISZERO;
			break;
		case Token::GT:
			m_context << (isSigned ? eth::Instruction::SGT : eth::Instruction::GT);
			break;
		case Token::LT:
			m_context << (isSigned ? eth::Instruction::SLT : eth::Instruction::LT);
			break;
		default:
			BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Unknown comparison operator."));
		}
	}
}

void ExpressionCompiler::appendOrdinaryBinaryOperatorCode(Token::Value _operator, Type const& _type)
{
	if (Token::isArithmeticOp(_operator))
		appendArithmeticOperatorCode(_operator, _type);
	else if (Token::isBitOp(_operator))
		appendBitOperatorCode(_operator);
	else if (Token::isShiftOp(_operator))
		appendShiftOperatorCode(_operator);
	else
		BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Unknown binary operator."));
}

void ExpressionCompiler::appendArithmeticOperatorCode(Token::Value _operator, Type const& _type)
{
	IntegerType const& type = dynamic_cast<IntegerType const&>(_type);
	bool const isSigned = type.isSigned();

	switch (_operator)
	{
	case Token::ADD:
		m_context << eth::Instruction::ADD;
		break;
	case Token::SUB:
		m_context << eth::Instruction::SUB;
		break;
	case Token::MUL:
		m_context << eth::Instruction::MUL;
		break;
	case Token::DIV:
		m_context  << (isSigned ? eth::Instruction::SDIV : eth::Instruction::DIV);
		break;
	case Token::MOD:
		m_context << (isSigned ? eth::Instruction::SMOD : eth::Instruction::MOD);
		break;
	default:
		BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Unknown arithmetic operator."));
	}
}

void ExpressionCompiler::appendBitOperatorCode(Token::Value _operator)
{
	switch (_operator)
	{
	case Token::BIT_OR:
		m_context << eth::Instruction::OR;
		break;
	case Token::BIT_AND:
		m_context << eth::Instruction::AND;
		break;
	case Token::BIT_XOR:
		m_context << eth::Instruction::XOR;
		break;
	default:
		BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Unknown bit operator."));
	}
}

void ExpressionCompiler::appendShiftOperatorCode(Token::Value _operator)
{
	BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Shift operators not yet implemented."));
	switch (_operator)
	{
	case Token::SHL:
		break;
	case Token::SAR:
		break;
	default:
		BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Unknown shift operator."));
	}
}

void ExpressionCompiler::appendTypeConversion(Type const& _typeOnStack, Type const& _targetType, bool _cleanupNeeded)
{
	// For a type extension, we need to remove all higher-order bits that we might have ignored in
	// previous operations.
	// @todo: store in the AST whether the operand might have "dirty" higher order bits

	if (_typeOnStack == _targetType && !_cleanupNeeded)
		return;
	if (_typeOnStack.getCategory() == Type::Category::INTEGER)
		appendHighBitsCleanup(dynamic_cast<IntegerType const&>(_typeOnStack));
	else if (_typeOnStack != _targetType)
		// All other types should not be convertible to non-equal types.
		BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Invalid type conversion requested."));
}

void ExpressionCompiler::appendHighBitsCleanup(IntegerType const& _typeOnStack)
{
	if (_typeOnStack.getNumBits() == 256)
		return;
	else if (_typeOnStack.isSigned())
		m_context << u256(_typeOnStack.getNumBits() / 8 - 1) << eth::Instruction::SIGNEXTEND;
	else
		m_context << ((u256(1) << _typeOnStack.getNumBits()) - 1) << eth::Instruction::AND;
}

void ExpressionCompiler::LValue::retrieveValue(Expression const& _expression, bool _remove) const
{
	switch (m_type)
	{
	case STACK:
	{
		unsigned stackPos = m_context->baseToCurrentStackOffset(unsigned(m_baseStackOffset));
		if (stackPos >= 15) //@todo correct this by fetching earlier or moving to memory
			BOOST_THROW_EXCEPTION(CompilerError() << errinfo_sourceLocation(_expression.getLocation())
												  << errinfo_comment("Stack too deep."));
		*m_context << eth::dupInstruction(stackPos + 1);
		break;
	}
	case STORAGE:
		if (!_expression.getType()->isValueType())
			break; // no distinction between value and reference for non-value types
		if (!_remove)
			*m_context << eth::Instruction::DUP1;
		*m_context << eth::Instruction::SLOAD;
		break;
	case MEMORY:
		if (!_expression.getType()->isValueType())
			break; // no distinction between value and reference for non-value types
		BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_sourceLocation(_expression.getLocation())
													  << errinfo_comment("Location type not yet implemented."));
		break;
	default:
		BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_sourceLocation(_expression.getLocation())
													  << errinfo_comment("Unsupported location type."));
		break;
	}
}

void ExpressionCompiler::LValue::storeValue(Expression const& _expression, bool _move) const
{
	switch (m_type)
	{
	case STACK:
	{
		unsigned stackPos = m_context->baseToCurrentStackOffset(unsigned(m_baseStackOffset));
		if (stackPos > 16)
			BOOST_THROW_EXCEPTION(CompilerError() << errinfo_sourceLocation(_expression.getLocation())
												  << errinfo_comment("Stack too deep."));
		else if (stackPos > 0)
			*m_context << eth::swapInstruction(stackPos) << eth::Instruction::POP;
		if (!_move)
			retrieveValue(_expression);
		break;
	}
	case LValue::STORAGE:
		if (!_expression.getType()->isValueType())
			break; // no distinction between value and reference for non-value types
		if (!_move)
			*m_context << eth::Instruction::DUP2 << eth::Instruction::SWAP1;
		*m_context << eth::Instruction::SSTORE;
		break;
	case LValue::MEMORY:
		if (!_expression.getType()->isValueType())
			break; // no distinction between value and reference for non-value types
		BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_sourceLocation(_expression.getLocation())
													  << errinfo_comment("Location type not yet implemented."));
		break;
	default:
		BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_sourceLocation(_expression.getLocation())
													  << errinfo_comment("Unsupported location type."));
		break;
	}
}

void ExpressionCompiler::LValue::retrieveValueIfLValueNotRequested(Expression const& _expression)
{
	if (!_expression.lvalueRequested())
	{
		retrieveValue(_expression, true);
		reset();
	}
}

void ExpressionCompiler::LValue::fromIdentifier(Identifier const& _identifier, Declaration const& _declaration)
{
	if (m_context->isLocalVariable(&_declaration))
	{
		m_type = STACK;
		m_baseStackOffset = m_context->getBaseStackOffsetOfVariable(_declaration);
	}
	else if (m_context->isStateVariable(&_declaration))
	{
		m_type = STORAGE;
		*m_context << m_context->getStorageLocationOfVariable(_declaration);
	}
	else
		BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_sourceLocation(_identifier.getLocation())
													  << errinfo_comment("Identifier type not supported or identifier not found."));
}

}
}