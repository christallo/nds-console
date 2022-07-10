#include "nscript.h"

std::string NScript::Node::toString()
{
  std::string temp;

  switch (kind)
  {
    case NodeKind::Num:         return cutTrailingZeros(std::to_string(value.num));
    case NodeKind::String:      return "'" + Parser::escapedToEscapes(value.str) + "'";
    case NodeKind::Bin:         return value.bin->left.toString() + " " + value.bin->op.toString() + " " + value.bin->right.toString();
    case NodeKind::Una:         return value.una->op.toString() + value.una->term.toString();
    case NodeKind::Assign:      return value.assign->name.toString() + " = " + value.assign->expr.toString();

    case NodeKind::Call:
      for (uint64_t i = 0; i < value.call->args.size(); i++)
      {
        // when this is not the first arg
        if (i > 0)
          temp.append(", ");

        temp.append(value.call->args[i].toString());
      }

      return value.call->name.toString() + "(" + temp + ")";

    case NodeKind::Plus:
    case NodeKind::Minus:
    case NodeKind::Star:
    case NodeKind::Slash:
    case NodeKind::LPar:
    case NodeKind::RPar:
    case NodeKind::Comma:
    case NodeKind::Eq:
    case NodeKind::Bad:
    case NodeKind::None:
    case NodeKind::Identifier:  return value.str;
    case NodeKind::Eof:         return "<eof>";
  }

  panic("unimplemented Node::toString() for some NodeKind");
  return nullptr;
}

NScript::Node NScript::Parser::nextToken()
{
  // eating all the whitespaces (they have no meaning)
  eatWhitespaces();

  if (eof())
    return Node(NodeKind::Eof, curPos());
  
  auto c = curChar();
  auto t = Node(curPos());

  if (isIdentifierChar(c, true))
    t = convertToKeywordWhenPossible(collectIdentifierToken());
  else if (isNumChar(c, true))
    t = collectNumToken();
  else if (c == '\'')
    t = collectStringToken();
  else if (arrayContains({'+', '-', '*', '/', '(', ')', ',', '='}, c))
    t = Node(NodeKind(c), (NodeValue) { .str = cstringRealloc(std::string(1, c).c_str()) }, curPos());

  exprIndex++;
  return t;
}

NScript::Node NScript::Parser::collectStringToken()
{
  // eating first `'`
  exprIndex++;

  auto startPos = exprIndex - 1;
  auto seq      = collectSequence([this] {
    // any character except `'`, unless it's an escaped character
    return curChar() != '\'' || (curChar(-1) == '\\' && curChar(-2) != '\\');
  });
  auto pos = Position(startPos, exprIndex + 2);

  // eating the last char of string
  // moving to the last `'`
  exprIndex++;

  if (eof())
    throw Error({"unclosed string"}, Position(startPos, exprIndex));

  return Node(NodeKind::String, (NodeValue) { .str = cstringRealloc(escapesToEscaped(seq, pos).c_str()) }, pos);
}

NScript::Node NScript::Parser::collectNumToken()
{
  auto startPos = exprIndex;
  auto seq      = collectSequence([this] {
    return isNumChar(curChar(), false);
  });
  auto pos      = Position(startPos, exprIndex + 1);

  // inconsistent numbers like 0.0.1 or 1.2.3 etc
  if (countOccurrences(seq, '.') > 1)
    throw Error({"number cannot include more than one dot"}, pos);
  
  // when the user wrote something like 0. or 2. etc
  if (seq[seq.length() - 1] == '.')
    throw Error(
      {"number cannot end with a dot (correction: `", seq.substr(0, seq.length() - 1), "`)"},
      pos
    );
  
  auto value = (NodeValue) {
    .num = atof(seq.c_str())
  };

  // when the next char is an identifier, the user wrote something like 123hello or 123_
  if (!eof(+1) && isIdentifierChar(curChar(+1), false))
    throw Error(
      {"number cannot include part of identifier (correction: `", seq, " ", std::string(1, curChar(+1)), "...`)"},
      Position(pos.startPos, curPos(+1).endPos)
    );

  return Node(NodeKind::Num, value, pos);
}

NScript::Node NScript::Parser::convertToKeywordWhenPossible(Node token)
{
  if (token.kind != NodeKind::Identifier)
   return token;
  
  if (token.value.str == std::string("none"))
    token.kind = NodeKind::None;

  return token;
}

NScript::Node NScript::Parser::collectIdentifierToken()
{
  auto startPos = exprIndex;
  auto value    = (NodeValue) {
    .str = cstringRealloc(collectSequence([this] {
      return isIdentifierChar(curChar(), false);
    }).c_str())
  };

  return Node(NodeKind::Identifier, value, Position(startPos, exprIndex + 1));
}

std::string NScript::Parser::collectSequence(std::function<bool()> checker)
{
  auto r = std::string();

  // as long as it matches a certain character, adds the latter to the string
  while (!eof() && checker())
  {
    r.push_back(curChar());
    exprIndex++;
  }

  // going back to the last char of sequence
  exprIndex--;

  return r;
}

NScript::Node NScript::Parser::expectBinaryOrTerm(std::function<Node()> expector, std::vector<NodeKind> operators)
{
  auto left = expector();

  // as long as matches one of the required operators, collects the right value and replaces the left one with a BinNode
  while (!eofToken() && arrayContains(operators, curToken.kind))
  {
    auto op = getCurAndAdvance();
    auto right = expector();

    left = Node(NodeKind::Bin, (NodeValue) { .bin = new BinNode(left, right, op) }, Position(left.pos.startPos, right.pos.endPos));
  }

  return left;
}

NScript::Node NScript::Parser::expectTerm()
{
  Node op;
  Node term;

  switch (getCurAndAdvance().kind)
  {
    // simple token
    case NodeKind::Identifier:
    case NodeKind::Num:
    case NodeKind::String:
    case NodeKind::None:
      term = prevToken;
      break;

    // unary expression = +|- term
    case NodeKind::Plus:
    case NodeKind::Minus:
      op   = prevToken;
      term = expectTerm();
      term = Node(NodeKind::Una, (NodeValue) { .una = new UnaNode(term, op) }, Position(op.pos.startPos, term.pos.endPos));
      break;
    
    case NodeKind::LPar:
      term = expectExpression();
      expectTokenAndAdvance(NodeKind::RPar);
      break;
    
    default:
      throw Error({"unexpected token (found `", prevToken.toString(), "`)"}, prevToken.pos);
  }

  if (curToken.kind == NodeKind::LPar)
    term = collectCallNode(term);
  else if (curToken.kind == NodeKind::Eq)
    term = collectAssignNode(term);
  
  return term;
}

NScript::Node NScript::Parser::collectAssignNode(Node name)
{
  if (name.kind != NodeKind::Identifier)
    throw Error({"expected an identifier when assigning"}, name.pos);

  // eating `=`
  advance();
  auto expr = expectExpression();

  return Node(NodeKind::Assign, (NodeValue) { .assign = new AssignNode(name, expr) }, Position(name.pos.startPos, expr.pos.endPos));
}

NScript::Node NScript::Parser::collectCallNode(Node name)
{
  if (name.kind != NodeKind::Identifier && name.kind != NodeKind::String)
    throw Error({"expected string or identifier call name"}, name.pos);
  
  auto startPos = curToken.pos.startPos;
  auto args     = std::vector<Node>();

  // eating first `(`
  advance();

  while (true)
  {
    if (eofToken())
      throw Error({"unclosed call parameters list"}, Position(startPos, prevToken.pos.endPos));
    
    if (curToken.kind == NodeKind::RPar)
    {
      // eating last `)`
      advance();
      return Node(NodeKind::Call, (NodeValue) { .call = new CallNode(name, args) }, Position(name.pos.startPos, prevToken.pos.endPos));
    }
    
    // when this is not the first arg
    if (args.size() > 0)
      expectTokenAndAdvance(NodeKind::Comma);
    
    args.push_back(expectExpression());
  }
}

std::string NScript::Parser::escapesToEscaped(std::string s, Position pos)
{
  std::string t;

  for (uint64_t i = 0; i < s.length(); i++)
    if (s[i] == '\\')
    {
      t.push_back(escapeChar(s[i + 1], Position(pos.startPos + i, pos.startPos + i + 1)));

      // skipping the escape code
      i++;
    }
    else
      t.push_back(s[i]);
  
  return t;
}

NScript::Node NScript::Evaluator::expectType(Node node, NodeKind type, Position pos)
{
  if (node.kind != type)
    throw Error({"expected a value with type ", Node::kindToString(type), " (found ", Node::kindToString(node.kind), ")"}, pos);
  
  return node;
}

void NScript::Evaluator::expectArgsCount(CallNode call, uint64_t count)
{
  if (call.args.size() != count)
    throw Error({"expected args ", std::to_string(count), " (found ", std::to_string(call.args.size()), ")"}, call.name.pos);
}

NScript::Node NScript::Evaluator::builtinFloor(CallNode call)
{
  expectArgsCount(call, 1);

  // truncating the float value
  auto expr = expectType(evaluateNode(call.args[0]), NodeKind::Num, call.args[0].pos);
  expr.value.num = uint64_t(expr.value.num);

  return expr;
}

NScript::Node NScript::Evaluator::builtinPrint(CallNode call, Position pos)
{
  // printing all arguments without separation and flushing
  for (auto arg : call.args)
    iprintf("%s", arg.toString().c_str());
  
  fflush(stdout);
  return Node::none(pos);
}

NScript::Node NScript::Evaluator::evaluateCallProcess(CallNode call, Position pos)
{
  panic("evaluateCallProcess not implemented yet");
  return Node::none(pos);
}

NScript::Node NScript::Evaluator::evaluateCall(CallNode call, Position pos)
{
  // when the call's name is a string, searches for a process with that filename
  if (call.name.kind == NodeKind::String)
    return evaluateCallProcess(call, pos);
  
  // otherwise searches for a builtin function with that name
  auto name = std::string(call.name.value.str);

  if (name == "print")
    return builtinPrint(call, pos);
  else if (name == "floor")
    return builtinFloor(call);
  else
    throw Error({"unknown builtin function"}, call.name.pos);
  
  return Node::none(pos);
}

NScript::Node NScript::Evaluator::evaluateAssign(AssignNode assign, Position pos)
{
  auto name = std::string(assign.name.value.str);
  auto expr = evaluateNode(assign.expr);

  for (uint64_t i = 0; i < map.size(); i++)
    if (map[i].key == name)
    {
      // the variable is already declared (overwrites old value)
      map[i].val = expr;
      return Node::none(pos);
    }

  // the variable is not declared yet (appends a new definition)
  map.push_back(KeyPair<std::string, Node>(name, expr));
  return Node::none(pos);
}

NScript::Node NScript::Evaluator::evaluateUna(UnaNode una)
{
  auto term = evaluateNode(una.term);

  // unary can only be applied to numbers
  if (term.kind != NodeKind::Num)
    throw Error({"type `", Node::kindToString(term.kind), "` does not support unary `", Node::kindToString(una.op.kind), "`"}, term.pos);
  
  term.value.num *= una.op.kind == NodeKind::Minus ? -1 : +1;
  return term;
}

cstring_t NScript::Evaluator::evaluateOperationStr(Node op, cstring_t l, cstring_t r)
{
  // string only supports `+` op
  if (op.kind != NodeKind::Plus)
    throw Error({"string does not support bin `", Node::kindToString(op.kind), "`"}, op.pos);

  return cstringRealloc((std::string(l) + r).c_str());
}

float64 NScript::Evaluator::evaluateOperationNum(NodeKind op, float64 l, float64 r, Position rPos)
{
  switch (op)
  {
    case NodeKind::Plus:  return l + r;
    case NodeKind::Minus: return l - r;
    case NodeKind::Star:  return l * r;
    case NodeKind::Slash:
      if (r == 0)
        throw Error({"dividing by 0"}, rPos);

      return l / r;
    
    default: panic("unreachable"); return 0;
  }
}

NScript::Node NScript::Evaluator::evaluateBin(BinNode bin)
{
  auto left  = evaluateNode(bin.left);
  auto right = evaluateNode(bin.right);

  // every bin op can only be applied to values of same type
  if (left.kind != right.kind)
    throw Error(
      {"unkwnon bin `", bin.op.toString(), "` between different types (`", Node::kindToString(left.kind), "` and `", Node::kindToString(right.kind), "`)"},
      bin.op.pos
    );
  
  // recognizing the values' types
  switch (left.kind)
  {
    case NodeKind::Num:
      left.value.num = evaluateOperationNum(bin.op.kind, left.value.num, right.value.num, right.pos);
      break;
    
    case NodeKind::String:
      left.value.str = evaluateOperationStr(bin.op, left.value.str, right.value.str);
      break;

    default:
      throw Error(
        {"type `", Node::kindToString(left.kind), "` does not support bin"},
        bin.op.pos
      );
  }

  // the returning value is gonna have the same pos of the entire bin node
  left.pos.endPos = right.pos.endPos;
  return left;
}

NScript::Node NScript::Evaluator::evaluateIdentifier(Node identifier)
{
  for (const auto& kv : map)
    if (kv.key == std::string(identifier.value.str))
      return kv.val;
  
  throw Error({"unknown variable"}, identifier.pos);
}

NScript::Node NScript::Evaluator::evaluateNode(Node node)
{
  switch (node.kind)
  {
    case NodeKind::Num:
    case NodeKind::String:
    case NodeKind::None:       return node;
    case NodeKind::Bin:        return evaluateBin(*node.value.bin);
    case NodeKind::Una:        return evaluateUna(*node.value.una);
    case NodeKind::Identifier: return evaluateIdentifier(node);
    case NodeKind::Assign:     return evaluateAssign(*node.value.assign, node.pos);
    case NodeKind::Call:       return evaluateCall(*node.value.call, node.pos);
    default:                   panic("unimplemented evaluateNode for some NodeKind"); return Node::none(node.pos);
  }
}