#include "sccp_pass.h"

#include "../utils/util.h"
#include "../utils/exception.h"
#include "../utils/pos.h"

#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Support/raw_ostream.h"
#include "../utils/exception.h"
// contains ReplaceInstWithValue and ReplaceInstWithInst
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
//to get the macros for easy predecessor and successor iteration
#include "llvm/Support/CFG.h"

#include <deque>
#include <map>
#include <algorithm>
#include <iterator>

#define TRANSITION(X, Y) void Transition::X(Y)
#define BINOP llvm::Instruction::BinaryOps
#define VALPAIR(X,Y) std::pair<llvm::Value*, ConstantLattice>(X,Y)
#define BLOCKPAIR(X,Y) std::pair<llvm::BasicBlock*, Reachability>(X,Y)

using namespace llvm;

void ConstantTable::checkedInsert(std::pair<llvm::Value*, ConstantLattice> pair)
{
  auto it =  this->find(pair.first);
  if (it == this->end()) {
    std::map<llvm::Value*, ConstantLattice>::insert(pair);
    return;
  } else {
    assert_that(it->second.state <= pair.second.state, "Error: Descended in lattice");
    if (   it->second.state == pair.second.state
        && pair.second.state == value) {
      assert_that(pair.second.value == it->second.value, "Value changed?");
    }
    this->operator[](pair.first) = pair.second;
  }
}

SCCP_Pass::SCCP_Pass() : FunctionPass(ID) {}

bool SCCP_Pass::runOnFunction(llvm::Function &F) {
  #ifdef DEBUG
  F.dump();
  #endif
  // maps BasicBlocks to either reachable or unreachable
  std::map<llvm::BasicBlock*, Reachability> BlockMapping;

  std::for_each(
    F.begin(),
    F.end(),
    [&](llvm::Function::iterator function_basic_block) {
      BlockMapping[function_basic_block] = unreachable;
  });
 
  //Initialize the Transition object
  {
    Transition transMngr = Transition(F, BlockMapping);

    llvm::BasicBlock* curr;

    while((curr = transMngr.getNextBlock())){
      std::for_each(
          curr->begin(),
          curr->end(),
          [&](llvm::BasicBlock::iterator basic_block_inst){
          transMngr.visit(basic_block_inst);
          });
    }
    transMngr.tearDownInsts();
    transMngr.deleteDeadBlocks();
  }
 
  return true; // we modified the module
}

char SCCP_Pass::ID = 0;
static RegisterPass<SCCP_Pass> X("sccp", "SCCP Pass", false, false);

//##############################################################################
//##                              Transition Class                            ##
//##############################################################################

Transition::Transition( llvm::Function& F,
                        std::map<llvm::BasicBlock*, Reachability>& blockTable):
                        blockTable(blockTable) {
  //Put all BasicBlocks of the function into our working queue
 std::for_each(
    F.begin(),
    F.end(),
    [&](llvm::Function::iterator function_basic_block) {
      workQueue.push_back(function_basic_block);
  });
}

Transition::~Transition(){
 // this->tearDownInsts(); 
//  this->deleteDeadBlocks();
}

/*
 * Removes the first element from the queue to give it to the sccppass to do the
 * work. As a side effect, the queues size is decreased by one when the element
 * is removed.
 */
llvm::BasicBlock* Transition::getNextBlock() {
  if (workQueue.empty()) {
    return nullptr;
  }
  //save the first block to return it
  llvm::BasicBlock* currBlock = std::move(workQueue.front());
  workQueue.pop_front(); //remove it from the queue
  return currBlock;
}

/*
 * Visit one AllocaInst.  As we don't know what value we are allocating, we must
 * map the value to top if it is reachable.
 */
TRANSITION(visitAllocaInst,llvm::AllocaInst& alloc){
  auto block = alloc.getParent();
  auto reachability = this->getReachabilityElem(block);
  auto oldElem = this->getConstantLatticeElem(&alloc);

  if (reachability.state == LatticeState::bottom){ //the block is reachable
    ConstantLattice newElem; //make new element
    newElem.state = LatticeState::top; //set it to top
    //insert the element
    this->constantTable.checkedInsert(std::pair<llvm::Value*, ConstantLattice>(&alloc, newElem));
    if(newElem.state != oldElem.state) //enqueue successors if value changed
      this->enqueueCFGSuccessors(alloc);
  }else{
    //unreachable block -> leave info set to bottom and don't change it
    return;
  }
}

/*
 *
 */
TRANSITION(visitBinaryOperator, llvm::BinaryOperator& binOp) {
  //take the first two operands more should not be present
  llvm::Value *lhs= binOp.getOperand(0);
  llvm::Value *rhs= binOp.getOperand(1);

  //the old info computed before
  ConstantLattice oldInfo = this->getConstantLatticeElem(&binOp);
  ConstantLattice newInfo;

  //take the corresponding lattice values
  ConstantLattice lhsInfo = this->getConstantLatticeElem(lhs);
  ConstantLattice rhsInfo = this->getConstantLatticeElem(rhs);

  //check wether one value already is top so we cannot compute something
  if(lhsInfo.state == LatticeState::top || rhsInfo.state == LatticeState::top){
    //check if we have an And or an Or Instruction which we could evaluate 
    //short circuit:
    if(binOp.getOpcode() == BINOP::Or){
      //check if one of the values is != 0 so the whole exp is true
      if((lhsInfo.state != LatticeState::top && lhsInfo.value != 0) ||
         (rhsInfo.state != LatticeState::top && rhsInfo.value != 0)){
        newInfo.state = LatticeState::value;
        newInfo.value = 1;
    }else if(binOp.getOpcode() == BINOP::And) {
      //check if one value is 0 
      //the case that both are != 0 can't occur here as we know that 
      //at least one is top
      if((lhsInfo.state != LatticeState::top && lhsInfo.value == 0) ||
         (rhsInfo.state != LatticeState::top && rhsInfo.value == 0)){
        newInfo.state = LatticeState::value;
        newInfo.value = 0;
      }
    }else{
      // we really can set the value to top
      newInfo.state = LatticeState::top; 
    }
    //check for value modification
    if(newInfo.state != oldInfo.state || newInfo.value != oldInfo.value){ 
      this->constantTable.checkedInsert(std::pair<llvm::Value*,ConstantLattice>(&binOp,newInfo));
      this->enqueueCFGSuccessors(binOp); //add succesors to queue as we need to 
                                         //update them
    }
      return;
    }
  } 
  //none of our operands is a top element --> we know that both have a fixed 
  //value or one of them is bottom.
  
  newInfo.state = LatticeState::value;
 
  //handle bottom case first
  if(lhsInfo.state == LatticeState::bottom || rhsInfo.state == LatticeState::bottom){
    newInfo.state = LatticeState::bottom;
    if(newInfo.state == oldInfo.state)
      return;
    this->constantTable.checkedInsert(VALPAIR(&binOp, newInfo));
    this->enqueueCFGSuccessors(binOp);
  }

  //we definitively have values --> do computation
  switch(binOp.getOpcode()){
  case BINOP::Add:
    newInfo.value = lhsInfo.value + rhsInfo.value;
    break;
  case BINOP::Sub:
    newInfo.value = lhsInfo.value - rhsInfo.value;
    break;
  case BINOP::Mul:
    newInfo.value = lhsInfo.value * rhsInfo.value;
    break;
  case BINOP::Or:
    newInfo.value = lhsInfo.value || rhsInfo.value;
    break;
  case BINOP::And:
    newInfo.value = lhsInfo.value && rhsInfo.value;
    break;
  default:
    break; 
  }
  //check if the value was computed before
  if(newInfo.value != oldInfo.value){
    this->constantTable.checkedInsert(VALPAIR(&binOp,newInfo));
    //enqueue the successors for updating them
    this->enqueueCFGSuccessors(binOp);
  }
  return;
}

/*
 * A call instruction references another function. SCCP is an intraprocedural
 * analysis, so we don't know anything about the result --> map the value to top
 */
TRANSITION(visitCallInst, llvm::CallInst& call){
  auto val = this->getConstantLatticeElem(& call);
  if(val.state == LatticeState::top)
          return;
  val.state = LatticeState::top;
  this->constantTable.checkedInsert(VALPAIR(&call, val));
  this->enqueueCFGSuccessors(call);
  return;
}

/*
 * TODO: Make this one an on-the-fly optimize function as it does not affect the
 * lattice but casts to already achieved type can be removed
 * For the moment just map it to the value of its operand
 */
TRANSITION(visitCastInst, llvm::CastInst &cast) {
  auto op = cast.getOperand(0);
  auto info = this->getConstantLatticeElem(op);
  this->constantTable.checkedInsert(VALPAIR(&cast, info));
  return;
}

/*
 * As all memory operations we cannot decide what a GEP actually computes so
 * we set its value to top
 */
TRANSITION(visitGetElementPtrInst, llvm::GetElementPtrInst& gep){
  auto info = this->getConstantLatticeElem(&gep);
  //if it is already top continue and do not enqueue the successors as the value
  //does not change
  if (info.state == LatticeState::top)
          return;
  info.state = LatticeState::top;
  this->enqueueCFGSuccessors(gep);
}

/*
 * Compare instructions influence reachability of BBs.
 */
TRANSITION(visitICmpInst, llvm::ICmpInst &cmp){
  auto lhs = cmp.getOperand(0);
  auto rhs = cmp.getOperand(1);

  //take the values
  auto lhsInfo = this->getConstantLatticeElem(lhs);
  auto rhsInfo = this->getConstantLatticeElem(rhs);

  auto oldInfo = this->getConstantLatticeElem(&cmp);
  ConstantLattice newInfo;
  //if one is top --> we cannot decide it
  if(lhsInfo.state == LatticeState::top || rhsInfo.state == LatticeState::top){
    if(oldInfo.state == LatticeState::top)
            return;
    else{
      newInfo.state = LatticeState::top;
      constantTable.checkedInsert(VALPAIR(&cmp, newInfo));
      this->enqueueCFGSuccessors(cmp);
    }
  }

  //if one is bottom leave it at bottom
  if(lhsInfo.state == LatticeState::bottom || rhsInfo.state == LatticeState::bottom){
    if(oldInfo.state == LatticeState::bottom)
            return;
    else{
      newInfo.state = LatticeState::bottom;
      constantTable.checkedInsert(VALPAIR(&cmp, newInfo));
      this->enqueueCFGSuccessors(cmp);
    }
  }


  //none is top or botttom --> compute the value
  newInfo.state = LatticeState::value;
  switch(cmp.getSignedPredicate()){
  case llvm::CmpInst::Predicate::ICMP_EQ:
    newInfo.value = (lhsInfo.value == rhsInfo.value);
  case llvm::CmpInst::Predicate::ICMP_NE:
    newInfo.value = (lhsInfo.value != rhsInfo.value);
  case llvm::CmpInst::Predicate::ICMP_SLT:
    newInfo.value = (lhsInfo.value < rhsInfo.value);
  default://FIXME: Maybe map unsupported ops to top
    return;
  }

}

/*
 * Same as visitGetElementPtrInst
 */
TRANSITION(visitLoadInst, llvm::LoadInst& load){
  auto info = this->getConstantLatticeElem(&load);
  //if it is already top continue do not enqueue the successors as the value
  //does not change
  if (info.state == LatticeState::top)
          return;
  info.state = LatticeState::top;
  this->enqueueCFGSuccessors(load);
}

/*
 * Same as visitGetElementPtrInst
 */
TRANSITION(visitStoreInst, llvm::StoreInst& store){
   auto val = store.getValueOperand();
   auto info = this->getConstantLatticeElem(&store);
   if(llvm::isa<llvm::ConstantInt>(val)){
     ConstantLattice newInfo;
     auto asInt = llvm::cast<llvm::ConstantInt>(val);
     newInfo.state = LatticeState::value;
     newInfo.value = asInt->getLimitedValue();
     this->constantTable.checkedInsert(VALPAIR(&store, info));
     if(newInfo.value != info.value || newInfo.state != info.state)
       this->enqueueCFGSuccessors(store);
   }else{
  //if it is already top continue and do not enqueue the successors as the value
  //does not change
  if (info.state == LatticeState::top)
          return;
  info.state = LatticeState::top;
  this->enqueueCFGSuccessors(store);
   }
}

/*
 * Checks if some predecessor values are dead. If there is only a single input
 * edge or value left, we can be sure that we have this value
 */
TRANSITION(visitPHINode, llvm::PHINode &phi){
  //get the old and new infos
  ConstantLattice oldInfo = this->getConstantLatticeElem(&phi);
  ConstantLattice newInfo;

  //get the values of the incoming edges
  int numOfIncomingVals = phi.getNumIncomingValues();
  std::vector<ConstantLattice> incomingValues;
  for(int i = 0; i < numOfIncomingVals; ++i){
    incomingValues.push_back(this->getConstantLatticeElem(phi.getIncomingValue(i)));
  }

  //check if we can simplify to a single value
  bool canBeValue = true;
  std::vector<int> values {};
  for(ConstantLattice elem : incomingValues){
    if(! canBeValue) //finish if we can be sure that it cannot be a single value
            break;
    canBeValue = canBeValue && ((elem.state == LatticeState::bottom)
                             || (elem.state == LatticeState::value));
    if(elem.state == LatticeState::value)
      values.push_back(elem.value);
  }
  //if we can make the phi a single value, compute it by iteration
  if(canBeValue){
    int res = values[0];
    bool allSame = true;
    for(int val : values){
      if(val == res)
        continue;
      else{
        allSame = false;
        break;
      }
      //if not all values are the same, we need to create a top value
      if(allSame){
        newInfo.state = LatticeState::value;
        newInfo.value = res;
        if(newInfo.state != oldInfo.state || newInfo.value != oldInfo.value){
        this->constantTable.checkedInsert(VALPAIR(&phi,newInfo));
        this->enqueueCFGSuccessors(phi);
        }
      }
    }
  }
  //create the top value and add it if the value would change
  newInfo.state = LatticeState::top;
  if(newInfo.state != oldInfo.state){
    this->constantTable.checkedInsert(VALPAIR(&phi, newInfo));
    this->enqueueCFGSuccessors(phi); 
  }
}

/*
 * Marks the target block reachable. It should not have a corresponding value!
 */
TRANSITION(visitBranchInst, llvm::BranchInst &branch){
  if(branch.isUnconditional()){ //unconditional branch --> successor reachable
    auto succ = branch.getSuccessor(0);
    auto info = this->getReachabilityElem(succ);
    if(info.state == reachable.state)
            return;
    this->blockTable.insert(BLOCKPAIR(succ, reachable));
  }else{ //conditional branch -->get value and decide based on it
    auto trueSucc = branch.getSuccessor(0);
    auto falseSucc = branch.getSuccessor(1);
    auto condVal = this->getConstantLatticeElem(branch.getCondition());
    if(condVal.state == LatticeState::value){ //we can decide which one is reachable
      if (condVal.value == 0){
        //false succ is reachable don't modify trueSucc as it could be reachable
        //from another block
        auto falseInfo = this->getReachabilityElem(falseSucc);
        if (falseInfo.state == reachable.state)//only modify if already reachable
          return;
        this->blockTable.insert(BLOCKPAIR(falseSucc, reachable));
      }else{
        //true succ is reachable don't modify falseSucc as it could be reachable
        //from another block
        auto trueInfo = this->getReachabilityElem(trueSucc);
        if (trueInfo.state == reachable.state)
          return;
        this->blockTable.insert(BLOCKPAIR(trueSucc, reachable));
      }
      this->constantTable.checkedInsert(VALPAIR(&branch, condVal));
    }else{ //both can be reachable modify both if not already reachable
      auto trueInfo = this->getReachabilityElem(trueSucc);
      auto falseInfo = this->getReachabilityElem(falseSucc);
      if (trueInfo.state != reachable.state){
        this->blockTable.insert(BLOCKPAIR(trueSucc, reachable));
      }
      if(falseInfo.state != reachable.state){
        this->blockTable.insert(BLOCKPAIR(falseSucc, reachable));
      }
    }
  }
}

/*
 * Check if operand value could be computed. If yes, map this instruction to 
 * this value
 */
TRANSITION(visitReturnInst, llvm::ReturnInst &ret){
  auto operand = ret.getOperand(0);
  auto info = this->getConstantLatticeElem(operand);
  constantTable.checkedInsert(VALPAIR(&ret, info));
  //WARNING: do not enqueue CFG successors of return! this would mean stepping
  //into another function
}

/*
 * Enqueues all CFG successors of a given instruction by taking its 
 * use chain and adding the uses parents
 */
void Transition::enqueueCFGSuccessors(llvm::Instruction &inst){
  for(auto use_it=inst.use_begin(); use_it != inst.use_end(); ++use_it){
    auto useObj = (*use_it);
    if(llvm::isa<llvm::Instruction>(useObj)){
    llvm::Instruction* use = llvm::cast<Instruction>(useObj);
    auto block = use->getParent();
    Reachability reach = this->getReachabilityElem(block);
    if(reach.state == LatticeState::bottom) //Top means Reachable
      workQueue.push_back(block);
    }
  }
}

ConstantLattice Transition::getConstantLatticeElem(llvm::Value* val){
  //first check if we are dealing with a constant
  if(llvm::isa<llvm::ConstantInt>(val)){
    auto asConst = llvm::cast<llvm::ConstantInt>(val);
    int value = asConst->getLimitedValue();
    ConstantLattice info;
    info.state = LatticeState::value;
    info.value = value;
//    this->constantTable.checkedInsert(VALPAIR(val,info)); TODO: Needed or not?
    return info;
  }else{     // it is no constant--> real op --> try to get it from the table
    auto it = this->constantTable.find(val);
    if(it != this->constantTable.end()) // we already computed the value
      return (*it).second;
    else{ //the values was not initialized--> this value was not used before
          //return the bottom element and map the value to it
      ConstantLattice constant;
      constant.state = LatticeState::bottom;
      constant.value = 0;
      this->constantTable.checkedInsert(VALPAIR(val,constant));
      return constant;
    }
  }
}

Reachability Transition::getReachabilityElem(llvm::BasicBlock* block){
  auto it = this->blockTable.find(block);
  if (it != this->blockTable.end())
    return (*it).second;
  //dead code should begin here
  Pos pos("Optimization Error",1337,1337);
  throw new CompilerException("Oh, internal error in blockTable. Missing obj.", pos);
}

void Transition::tearDownInsts(){
  std::for_each(
    this->constantTable.begin(),
    this->constantTable.end(),
    [&](decltype(*this->constantTable.begin()) inst_const_pair){
      auto info = inst_const_pair.second;
      auto val = inst_const_pair.first;
      if(info.state != LatticeState::top){
        auto asInst = llvm::cast<llvm::Instruction>(val);
        if(info.state == LatticeState::bottom){
        #ifdef DEBUG
        //TODO: check all succesors for "bottom"
        #endif
        //reverse the DU chain, to start with deletion of last use
        std::vector<llvm::Value*> duReverted;
        std::for_each(
          val->use_begin(),
          val->use_end(),
          [&](llvm::Value* use){
            duReverted.insert(duReverted.begin(), use);
            // prevent double deletion without invalidating the iterator
            this->constantTable[use].state = top;
            }
          );
         std::for_each(
           duReverted.begin(),
           duReverted.end(),
           [&](llvm::Value* use){
            auto asInst = llvm::cast<llvm::Instruction>(use);
            asInst->removeFromParent();
          });
          //finally remove the instruction itself
          asInst->removeFromParent();
        }
        if(llvm::isa<llvm::BranchInst>(val)){
          auto asBranch = llvm::cast<llvm::BranchInst>(val);
          llvm::BasicBlock* succ = nullptr;
          if(info.value == 0)
            succ = asBranch->getSuccessor(1);
          else
            succ= asBranch->getSuccessor(0);
          val->replaceAllUsesWith(llvm::BranchInst::Create(succ)); 
        }else{
        auto type = llvm::Type::getInt32Ty(llvm::getGlobalContext());
        auto constVal = llvm::ConstantInt::get(type, info.value, true);
        llvm::BasicBlock::iterator ii (asInst);
        ReplaceInstWithValue(asInst->getParent()->getInstList(),ii,constVal);
        }
      }
    });
}

void Transition::deleteDeadBlocks(){

}


