#include "instruction.h"
#include "simulator.h"
#include "tile_manager.h"
#include "tile.h"
#include "core_model.h"
#include "branch_predictor.h"

// Instruction

Instruction::StaticInstructionCosts Instruction::m_instruction_costs;

Instruction::Instruction(InstructionType type, OperandList &operands)
   : m_type(type)
   , m_addr(0)
   , m_operands(operands)
{
}

Instruction::Instruction(InstructionType type)
   : m_type(type)
   , m_addr(0)
{
}

InstructionType Instruction::getType()
{
    return m_type;
}

UInt64 Instruction::getCost()
{
   LOG_ASSERT_ERROR(m_type < MAX_INSTRUCTION_COUNT, "Unknown instruction type: %d", m_type);
   return Instruction::m_instruction_costs[m_type]; 
}

void Instruction::initializeStaticInstructionModel()
{
   m_instruction_costs.resize(MAX_INSTRUCTION_COUNT);
   for(unsigned int i = 0; i < MAX_INSTRUCTION_COUNT; i++)
   {
       char key_name [1024];
       snprintf(key_name, 1024, "perf_model/core/static_instruction_costs/%s", INSTRUCTION_NAMES[i]);
       UInt32 instruction_cost = Sim()->getCfg()->getInt(key_name, 0);
       m_instruction_costs[i] = instruction_cost;
   }
}

// DynamicInstruction

DynamicInstruction::DynamicInstruction(UInt64 cost, InstructionType type)
   : Instruction(type)
   , m_cost(cost)
{
}

DynamicInstruction::~DynamicInstruction()
{
}

UInt64 DynamicInstruction::getCost()
{
   return m_cost;
}

// StringInstruction

StringInstruction::StringInstruction(OperandList &ops)
   : Instruction(INST_STRING, ops)
{
}

UInt64 StringInstruction::getCost()
{
   // dequeue mem ops until we hit the final marker, then check count
   CoreModel *perf = Sim()->getTileManager()->getCurrentCore()->getPerformanceModel();
   UInt32 count = 0;
   UInt64 cost = 0;
   DynamicInstructionInfo* i;

   while (true)
   {
      i = &perf->getDynamicInstructionInfo();

      if (i->type == DynamicInstructionInfo::STRING)
         break;

      LOG_ASSERT_ERROR(i->type == DynamicInstructionInfo::MEMORY_READ,
                       "Expected memory read in string instruction (or STRING).");

      cost += i->memory_info.latency;

      ++count;
      perf->popDynamicInstructionInfo();
   }

   LOG_ASSERT_ERROR(count == i->string_info.num_ops,
                    "Number of mem ops in queue doesn't match number in string instruction.");
   perf->popDynamicInstructionInfo();

   return cost;
}

// SyncInstruction

SyncInstruction::SyncInstruction(UInt64 cost)
   : DynamicInstruction(cost, INST_SYNC)
{ }

// SpawnInstruction

SpawnInstruction::SpawnInstruction(UInt64 time)
   : Instruction(INST_SPAWN)
   , m_time(time)
{ }

UInt64 SpawnInstruction::getCost()
{
   CoreModel *perf = Sim()->getTileManager()->getCurrentCore()->getPerformanceModel();
   perf->setCycleCount(m_time);
   throw CoreModel::AbortInstructionException(); // exit out of handleInstruction
}

// BranchInstruction

BranchInstruction::BranchInstruction(OperandList &l)
   : Instruction(INST_BRANCH, l)
{ }

UInt64 BranchInstruction::getCost()
{
   CoreModel *perf = Sim()->getTileManager()->getCurrentCore()->getPerformanceModel();
   BranchPredictor *bp = perf->getBranchPredictor();

   DynamicInstructionInfo &i = perf->getDynamicInstructionInfo();
   LOG_ASSERT_ERROR(i.type == DynamicInstructionInfo::BRANCH, "type(%u)", i.type);

   // branch prediction not modeled
   if (bp == NULL)
   {
      perf->popDynamicInstructionInfo();
      return 1;
   }

   bool prediction = bp->predict(getAddress(), i.branch_info.target);
   bool correct = (prediction == i.branch_info.taken);

   bp->update(prediction, i.branch_info.taken, getAddress(), i.branch_info.target);
   UInt64 cost = correct ? 1 : bp->getMispredictPenalty();
      
   perf->popDynamicInstructionInfo();
   return cost;
}
