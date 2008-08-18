//
// Transformations for begin, cobegin, and on statements
//

#include "astutil.h"
#include "expr.h"
#include "passes.h"
#include "stmt.h"
#include "symbol.h"
#include "stringutil.h"
#include "driver.h"
#include "files.h"


// Package args into a class and call a wrapper function with that
// object. The wrapper function will then call the function
// created by the previous parallel pass. This is a way to pass along
// multiple args through the limitation of one arg in the runtime's
// thread creation interface. 
static void
bundleArgs(CallExpr* fcall) {
  SET_LINENO(fcall);
  ModuleSymbol* mod = fcall->getModule();
  FnSymbol* fn = fcall->isResolved();

  // create a new class to capture refs to locals
  ClassType* ctype = new ClassType( CLASS_CLASS);
  TypeSymbol* new_c = new TypeSymbol( astr("_class_locals", 
                                           fn->name),
                                      ctype);
  new_c->addPragma(PRAG_NO_OBJECT);
  new_c->addPragma(PRAG_NO_WIDE_CLASS);

  // add the function args as fields in the class
  int i = 1;
  bool first = true;
  for_actuals(arg, fcall) {
    if (fn->hasPragma(PRAG_ON) && first) {
      first = false;
      continue;
    }
    SymExpr *s = toSymExpr(arg);
    Symbol  *var = s->var; // arg or var
    var->isConcurrent = true;
    VarSymbol* field = new VarSymbol(astr("_", istr(i), "_", var->name), var->type);
    ctype->fields.insertAtTail(new DefExpr(field));
    i++;
  }
  mod->block->insertAtHead(new DefExpr(new_c));
    
  // create the class variable instance and allocate it
  VarSymbol *tempc = new VarSymbol( astr( "_args_for", 
                                          fn->name),
                                    ctype);
  fcall->insertBefore( new DefExpr( tempc));
  CallExpr *tempc_alloc = new CallExpr( PRIMITIVE_CHPL_ALLOC_PERMIT_ZERO,
                                        ctype->symbol,
                                        new_StringSymbol( astr( "instance of class ", ctype->symbol->name)));
  fcall->insertBefore( new CallExpr( PRIMITIVE_MOVE,
                                     tempc,
                                     tempc_alloc));
  
  // set the references in the class instance
  i = 1;
  first = true;
  for_actuals(arg, fcall) {
    if (fn->hasPragma(PRAG_ON) && first) {
      first = false;
      continue;
    }
    SymExpr *s = toSymExpr(arg);
    Symbol  *var = s->var; // var or arg
    CallExpr *setc=new CallExpr(PRIMITIVE_SET_MEMBER,
                                tempc,
                                ctype->getField(i),
                                var);
    fcall->insertBefore( setc);
    i++;
  }
  
  // create wrapper-function that uses the class instance

  FnSymbol *wrap_fn = new FnSymbol( astr("wrap", fn->name));
  DefExpr  *fcall_def= (toSymExpr( fcall->baseExpr))->var->defPoint;
  if (fn->hasPragma(PRAG_ON)) {
    wrap_fn->addPragma(PRAG_ON_BLOCK);
    ArgSymbol* locale = new ArgSymbol(INTENT_BLANK, "_dummy_locale_arg", dtInt[INT_SIZE_32]);
    wrap_fn->insertFormalAtTail(locale);
  } else if (fn->hasPragma(PRAG_COBEGIN_OR_COFORALL)) {
    wrap_fn->addPragma(PRAG_COBEGIN_OR_COFORALL_BLOCK);
  }
  ArgSymbol *wrap_c = new ArgSymbol( INTENT_BLANK, "c", ctype);
  wrap_fn->insertFormalAtTail( wrap_c);

  mod->block->insertAtTail(new DefExpr(wrap_fn));
  if (fn->hasPragma(PRAG_ON)) {
    fcall->insertBefore(new CallExpr(wrap_fn, fcall->get(1)->remove(), tempc));
  } else
    fcall->insertBefore(new CallExpr(wrap_fn, tempc));

  if (fn->hasPragma(PRAG_BEGIN) || fn->hasPragma(PRAG_COBEGIN_OR_COFORALL)) {
    if (fn->hasPragma(PRAG_BEGIN))
      wrap_fn->addPragma(PRAG_BEGIN_BLOCK);
    wrap_fn->insertAtHead(new CallExpr(PRIMITIVE_THREAD_INIT));
  }
    
  // translate the original cobegin function
  CallExpr *new_cofn = new CallExpr( (toSymExpr(fcall->baseExpr))->var);
  if (fn->hasPragma(PRAG_ON))
    new_cofn->insertAtTail(new_IntSymbol(0)); // bogus actual
  for_fields(field, ctype) {  // insert args

    VarSymbol* tmp = new VarSymbol("_tmp", field->type);
    tmp->isCompilerTemp = true;
    wrap_fn->insertAtTail(new DefExpr(tmp));
    wrap_fn->insertAtTail(
      new CallExpr(PRIMITIVE_MOVE, tmp,
        new CallExpr(PRIMITIVE_GET_MEMBER_VALUE, wrap_c, field)));
    new_cofn->insertAtTail(tmp);
  }

  wrap_fn->retType = dtVoid;
  wrap_fn->insertAtTail(new_cofn);     // add new call
  if (fn->hasPragma(PRAG_ON))
    fcall->insertAfter(new CallExpr(PRIMITIVE_CHPL_FREE, tempc));
  else
    wrap_fn->insertAtTail(new CallExpr(PRIMITIVE_CHPL_FREE, wrap_c));

  fcall->remove();                     // rm orig. call
  fcall_def->remove();                 // move orig. def
  mod->block->insertAtTail(fcall_def); // to top-level
  normalize(wrap_fn);
}


static void
insertEndCount(FnSymbol* fn,
               Type* endCountType,
               Vec<FnSymbol*>& queue,
               Map<FnSymbol*,Symbol*>& endCountMap) {
  if (fn == chpl_main) {
    VarSymbol* var = new VarSymbol("_endCount", endCountType);
    var->isCompilerTemp = true;
    fn->insertAtHead(new DefExpr(var));
    endCountMap.put(fn, var);
    queue.add(fn);
  } else {
    ArgSymbol* arg = new ArgSymbol(INTENT_BLANK, "_endCount", endCountType);
    fn->insertFormalAtTail(arg);
    VarSymbol* var = new VarSymbol("_endCount", endCountType);
    var->isCompilerTemp = true;
    fn->insertAtHead(new CallExpr(PRIMITIVE_MOVE, var, arg));
    fn->insertAtHead(new DefExpr(var));
    endCountMap.put(fn, var);
    queue.add(fn);
  }
}


void
parallel(void) {
  compute_call_sites();

  Vec<FnSymbol*> queue;
  Map<FnSymbol*,Symbol*> endCountMap;

  forv_Vec(BaseAST, ast, gAsts) {
    if (CallExpr * call = toCallExpr(ast)) {
      if (call->isPrimitive(PRIMITIVE_GET_END_COUNT)) {
        FnSymbol* pfn = call->getFunction();
        if (!endCountMap.get(pfn))
          insertEndCount(pfn, call->typeInfo(), queue, endCountMap);
        call->replace(new SymExpr(endCountMap.get(pfn)));
      } else if (call->isPrimitive(PRIMITIVE_SET_END_COUNT)) {
        FnSymbol* pfn = call->getFunction();
        if (!endCountMap.get(pfn))
          insertEndCount(pfn, call->get(1)->typeInfo(), queue, endCountMap);
        call->replace(new CallExpr(PRIMITIVE_MOVE, endCountMap.get(pfn), call->get(1)->remove()));
      }
    }
  }

  forv_Vec(FnSymbol, fn, queue) {
    forv_Vec(CallExpr, call, *fn->calledBy) {
      Type* endCountType = endCountMap.get(fn)->type;
      FnSymbol* pfn = call->getFunction();
      if (!endCountMap.get(pfn))
        insertEndCount(pfn, endCountType, queue, endCountMap);
      call->insertAtTail(endCountMap.get(pfn));
    }
  }

  Vec<CallExpr*> calls;
  forv_Vec(BaseAST, ast, gAsts) {
    if (CallExpr* call = toCallExpr(ast))
      calls.add(call);
  }

  forv_Vec(CallExpr, call, calls) {
    if (call->isResolved() && (call->isResolved()->hasPragma(PRAG_ON) ||
                               call->isResolved()->hasPragma(PRAG_BEGIN) ||
                               call->isResolved()->hasPragma(PRAG_COBEGIN_OR_COFORALL)))
      bundleArgs(call);
  }
}


static void
buildWideClass(Type* type) {
  ClassType* wide = new ClassType(CLASS_RECORD);
  TypeSymbol* wts = new TypeSymbol(astr("_wide_", type->symbol->cname), wide);
  wts->addPragma(PRAG_WIDE_CLASS);
  theProgram->block->insertAtTail(new DefExpr(wts));
  wide->fields.insertAtTail(new DefExpr(new VarSymbol("locale", dtInt[INT_SIZE_32])));
  wide->fields.insertAtTail(new DefExpr(new VarSymbol("addr", type)));
  //
  // Strings need an extra field in their wide class to hold their length
  //
  if (type == dtString)
    wide->fields.insertAtTail(new DefExpr(new VarSymbol("size", dtInt[INT_SIZE_32])));
  wideClassMap.put(type, wide);

  //
  // build reference type for wide class type
  //
  ClassType* ref = new ClassType(CLASS_CLASS);
  TypeSymbol* rts = new TypeSymbol(astr("_ref_wide_", type->symbol->cname), ref);
  rts->addPragma(PRAG_REF);
  theProgram->block->insertAtTail(new DefExpr(rts));
  ref->fields.insertAtTail(new DefExpr(new VarSymbol("_val", type)));
  wide->refType = ref;
}


//
// The argument expr is a use of a wide reference. Insert a check to ensure
// that it is on the current locale, then drop its wideness by moving the
// addr field into a non-wide of otherwise the same type. Then, replace its
// use with the non-wide version.
//
static void insertLocalTemp(Expr* expr) {
  SymExpr* se = toSymExpr(expr);
  Expr* stmt = expr->getStmtExpr();
  INT_ASSERT(se && stmt);
  SET_LINENO(se);
  VarSymbol* var = new VarSymbol(astr("local_", se->var->name),
                                 se->var->type->getField("addr")->type);
  if (!fNoLocalChecks) {
    stmt->insertBefore(new CallExpr(PRIMITIVE_LOCAL_CHECK, se->copy()));
  }
  stmt->insertBefore(new DefExpr(var));
  stmt->insertBefore(new CallExpr(PRIMITIVE_LOCAL_DEREF, se->copy(), var));
  se->replace(new SymExpr(var));
}


//
// If call has the potential to cause communication, assert that the wide
// reference that might cause communication is local and remove its wide-ness
//
// The organization of this function follows the order of CallExpr::codegen()
// leaving out primitives that don't communicate.
//
static void localizeCall(CallExpr* call) {
  if (call->primitive) {
    switch (call->primitive->tag) {
    case PRIMITIVE_ARRAY_SET: /* Fallthru */
    case PRIMITIVE_ARRAY_SET_FIRST:
      if (call->get(1)->typeInfo()->symbol->hasPragma(PRAG_WIDE_CLASS)) {
        insertLocalTemp(call->get(1));
      }
      break;
    case PRIMITIVE_MOVE:
      if (CallExpr* rhs = toCallExpr(call->get(2))) {
        if (rhs->isPrimitive(PRIMITIVE_GET_LOCALEID)) {
          if (rhs->get(1)->typeInfo()->symbol->hasPragma(PRAG_WIDE)) {
            if (getValueType(rhs->get(1)->typeInfo()->getField("addr")->type)->symbol->hasPragma(PRAG_WIDE_CLASS)) {
              insertLocalTemp(rhs->get(1));
            }
          }
        } else if (rhs->isPrimitive(PRIMITIVE_GET_REF)) {
          if (rhs->get(1)->typeInfo()->symbol->hasPragma(PRAG_WIDE) ||
              rhs->get(1)->typeInfo()->symbol->hasPragma(PRAG_WIDE_CLASS)) {
            insertLocalTemp(rhs->get(1));
            if (!rhs->get(1)->typeInfo()->symbol->hasPragma(PRAG_REF))
              rhs->replace(rhs->get(1)->remove());
          }
        } else if (rhs->isPrimitive(PRIMITIVE_GET_MEMBER_VALUE)) {
          if (rhs->get(1)->typeInfo()->symbol->hasPragma(PRAG_WIDE) ||
              rhs->get(1)->typeInfo()->symbol->hasPragma(PRAG_WIDE_CLASS)) {
            SymExpr* sym = toSymExpr(rhs->get(2));
            INT_ASSERT(sym);
            if (!sym->var->hasPragma(PRAG_SUPER_CLASS)) {
              insertLocalTemp(rhs->get(1));
            }
          }
        } else if (rhs->isPrimitive(PRIMITIVE_ARRAY_GET) ||
                   rhs->isPrimitive(PRIMITIVE_ARRAY_GET_VALUE)) {
          if (rhs->get(1)->typeInfo()->symbol->hasPragma(PRAG_WIDE_CLASS)) {
            SymExpr* lhs = toSymExpr(call->get(1));
            Expr* stmt = call->getStmtExpr();
            INT_ASSERT(lhs && stmt);
            insertLocalTemp(rhs->get(1));
            VarSymbol* localVar = new VarSymbol(astr("local_", lhs->var->name),
                                    lhs->var->type->getField("addr")->type);
            stmt->insertBefore(new DefExpr(localVar));
            lhs->replace(new SymExpr(localVar));
            stmt->insertAfter(new CallExpr(PRIMITIVE_MOVE, lhs,
                                           new SymExpr(localVar)));
          }
        } else if (rhs->isPrimitive(PRIMITIVE_UNION_GETID)) {
          if (rhs->get(1)->typeInfo()->symbol->hasPragma(PRAG_WIDE)) {
            insertLocalTemp(rhs->get(1));
          }
        } else if (rhs->isPrimitive(PRIMITIVE_GETCID)) {
          if (rhs->get(1)->typeInfo()->symbol->hasPragma(PRAG_WIDE_CLASS)) {
            insertLocalTemp(rhs->get(1));
          }
        }
      } else if (call->get(1)->typeInfo()->symbol->hasPragma(PRAG_WIDE) &&
                 !call->get(2)->typeInfo()->symbol->hasPragma(PRAG_WIDE) &&
                 !call->get(2)->typeInfo()->symbol->hasPragma(PRAG_REF)) {
        insertLocalTemp(call->get(1));
      }
      break;
    case PRIMITIVE_DYNAMIC_CAST:
      if (call->get(2)->typeInfo()->symbol->hasPragma(PRAG_WIDE_CLASS)) {
        insertLocalTemp(call->get(2));
        if (call->get(1)->typeInfo()->symbol->hasPragma(PRAG_WIDE_CLASS) ||
            call->get(1)->typeInfo()->symbol->hasPragma(PRAG_WIDE)) {
          toSymExpr(call->get(1))->var->type = call->get(1)->typeInfo()->getField("addr")->type;
        }
      }
    break;
    case PRIMITIVE_SETCID:
      if (call->get(1)->typeInfo()->symbol->hasPragma(PRAG_WIDE_CLASS)) {
        insertLocalTemp(call->get(1));
      }
      break;
    case PRIMITIVE_UNION_SETID:
      if (call->get(1)->typeInfo()->symbol->hasPragma(PRAG_WIDE)) {
        insertLocalTemp(call->get(1));
      }
      break;
    case PRIMITIVE_SET_MEMBER:
      if (call->get(1)->typeInfo()->symbol->hasPragma(PRAG_WIDE_CLASS) ||
          call->get(1)->typeInfo()->symbol->hasPragma(PRAG_WIDE)) {
        insertLocalTemp(call->get(1));
      }
      break;
    default:
      break;
    }
  }
}


//
// Do a breadth first search starting from functions generated for local blocks
// for all function calls in each level of the search, if they directly cause
// communication, add a local temp that isn't wide. If it is a resolved call,
// meaning that it isn't a primitive or external function, clone it and add it
// to the queue of functions to handle at the next iteration of the BFS.
//
static void handleLocalBlocks() {
  Map<FnSymbol*, FnSymbol*> fnMap;
  Vec<FnSymbol*> localizeFns, next;

  forv_Vec(FnSymbol, fn, gFns) {
    if (fn->hasPragma(PRAG_LOCAL_BLOCK))
      localizeFns.add(fn);
  }
  Vec<FnSymbol*>* localizeFnsP = &localizeFns;
  Vec<FnSymbol*>* nextP = &next;

  while (localizeFnsP->n > 0) {
    forv_Vec(FnSymbol, fn, *localizeFnsP) {
      Vec<BaseAST*> asts;
      collect_asts(fn->body, asts);
      forv_Vec(BaseAST, ast, asts) {
        if (CallExpr* call = toCallExpr(ast)) {
          localizeCall(call);
          if (FnSymbol* fn = call->isResolved()) {
            if (FnSymbol* localFn = fnMap.get(fn)) {
              call->baseExpr->replace(new SymExpr(localFn));
            } else {
              if (!fn->hasPragma(PRAG_LOCALIZED)) {
                FnSymbol* copy = fn->copy();
                copy->name = astr("_local_", fn->name);
                copy->cname = astr("_local_", fn->cname);
                copy->addPragma(PRAG_LOCALIZED);
                fn->defPoint->insertBefore(new DefExpr(copy));
                fnMap.put(fn, copy);
                call->baseExpr->replace(new SymExpr(copy));
                nextP->add(copy);
              } else {
                INT_FATAL("localized function should already be in map", fn);
              }
            }
          }
        }
      }
    }
    Vec<FnSymbol*>* tmp = localizeFnsP;
    localizeFnsP = nextP;
    nextP = tmp;
    nextP->clear();
  } 
}


//
// change all classes into wide classes
// change all references into wide references
//
void
insertWideReferences(void) {
  if (fRuntime)
    return;

  FnSymbol* heapAllocateGlobals = new FnSymbol("_heapAllocateGlobals");
  heapAllocateGlobals->retType = dtVoid;
  theProgram->block->insertAtTail(new DefExpr(heapAllocateGlobals));
  heapAllocateGlobals->insertAtHead(new CallExpr(PRIMITIVE_ALLOC_GVR));

  if (fLocal) {
    heapAllocateGlobals->insertAtTail(new CallExpr(PRIMITIVE_RETURN, gVoid));
    return;
  }

  //
  // change dtNil into dtObject
  //
  forv_Vec(BaseAST, ast, gAsts) {
    if (DefExpr* def = toDefExpr(ast)) {
      if (FnSymbol* fn = toFnSymbol(def->sym)) {
        if (fn->retType == dtNil)
          fn->retType = dtObject;
      } else if (!isTypeSymbol(def->sym)) {
        if (def->sym != gNil && def->sym->type == dtNil)
          def->sym->type = dtObject;
      }
    }
  }

  wideClassMap.clear();

  //
  // build wide class type for every class type
  //
  forv_Vec(TypeSymbol, ts, gTypes) {
    ClassType* ct = toClassType(ts->type);
    if (ct && ct->classTag == CLASS_CLASS && !ts->hasPragma(PRAG_REF) && !ts->hasPragma(PRAG_NO_WIDE_CLASS)) {
      buildWideClass(ct);
    }
  }
  buildWideClass(dtString);
  //
  // change all classes into wide classes
  //
  forv_Vec(BaseAST, ast, gAsts) {
    if (DefExpr* def = toDefExpr(ast)) {

      //
      // do not widen literals or nil reference
      //
      if (VarSymbol* var = toVarSymbol(def->sym))
        if (var->immediate)
          continue;

      //
      // do not change class field in wide class type
      //
      if (TypeSymbol* ts = toTypeSymbol(def->parentSymbol))
        if (ts->hasPragma(PRAG_WIDE_CLASS))
          continue;

      //
      // do not change super class field - it's really a record
      //
      if (def->sym->hasPragma(PRAG_SUPER_CLASS))
        continue;

      if (FnSymbol* fn = toFnSymbol(def->sym)) {
        if (Type* wide = wideClassMap.get(fn->retType))
          if (!fn->hasPragma(PRAG_EXTERN))
            fn->retType = wide;
      } else if (!isTypeSymbol(def->sym)) {
        if (Type* wide = wideClassMap.get(def->sym->type)) {
          if (def->parentSymbol->hasPragma(PRAG_EXTERN)) {
            if (toArgSymbol(def->sym))
              continue; // don't change extern function's arguments
          }
          def->sym->type = wide;
        }
      }
    }
  }

  //
  // change arrays of classes into arrays of wide classes
  //
  forv_Vec(TypeSymbol, ts, gTypes) {
    if (ts->hasPragma(PRAG_DATA_CLASS)) {
      if (Type* nt = wideClassMap.get(toTypeSymbol(ts->type->substitutions.v[0].value)->type)) {
        ts->type->substitutions.v[0].value = nt->symbol;
      }
    }
  }

  wideRefMap.clear();

  //
  // build wide reference type for every reference type
  //
  forv_Vec(TypeSymbol, ts, gTypes) {
    if (ts->hasPragma(PRAG_REF)) {
      ClassType* wide = new ClassType(CLASS_RECORD);
      TypeSymbol* wts = new TypeSymbol(astr("_wide_", ts->cname), wide);
      wts->addPragma(PRAG_WIDE);
      theProgram->block->insertAtTail(new DefExpr(wts));
      wide->fields.insertAtTail(new DefExpr(new VarSymbol("locale", dtInt[INT_SIZE_32])));
      wide->fields.insertAtTail(new DefExpr(new VarSymbol("addr", ts->type)));
      wideRefMap.put(ts->type, wide);
    }
  }

  //
  // change all references into wide references
  //
  forv_Vec(BaseAST, ast, gAsts) {
    if (DefExpr* def = toDefExpr(ast)) {

      //
      // do not widen reference nil
      //
      if (def->sym == gNilRef)
        continue;

      //
      // do not change reference field in wide reference type
      //
      if (TypeSymbol* ts = toTypeSymbol(def->parentSymbol))
        if (ts->hasPragma(PRAG_WIDE))
          continue;

      //
      // do not change super field - it's really a record
      //
      if (def->sym->hasPragma(PRAG_SUPER_CLASS))
        continue;

      if (FnSymbol* fn = toFnSymbol(def->sym)) {
        if (Type* wide = wideRefMap.get(fn->retType))
          fn->retType = wide;
      } else if (!isTypeSymbol(def->sym)) {
        if (Type* wide = wideRefMap.get(def->sym->type))
          def->sym->type = wide;
      }
    }
  }

  //
  // Special case string literals passed to functions, set member primitives
  // and array element initializers by pushing them into temps first.
  //
  forv_Vec(BaseAST, ast, gAsts) {
    if (SymExpr* se = toSymExpr(ast)) {
      if (se->var->type == dtString) {
        if (VarSymbol* var = toVarSymbol(se->var)) {
          if (var->immediate) {
            if (CallExpr* call = toCallExpr(se->parentExpr)) {
              if (call->isResolved() && !call->isResolved()->hasPragma(PRAG_EXTERN)) {
                if (Type* type = actual_to_formal(se)->typeInfo()) {
                  VarSymbol* tmp = new VarSymbol("tmp", type);
                  call->getStmtExpr()->insertBefore(new DefExpr(tmp));
                  se->replace(new SymExpr(tmp));
                  call->getStmtExpr()->insertBefore(new CallExpr(PRIMITIVE_MOVE, tmp, se));
                }
              } else if (call->isPrimitive(PRIMITIVE_SET_MEMBER)) {
                if (SymExpr* wide = toSymExpr(call->get(2))) {
                  Type* type = wide->var->type;
                  VarSymbol* tmp = new VarSymbol("tmp", type);
                  call->getStmtExpr()->insertBefore(new DefExpr(tmp));
                  se->replace(new SymExpr(tmp));
                  call->getStmtExpr()->insertBefore(new CallExpr(PRIMITIVE_MOVE, tmp, se));
                }
              } else if (call->isPrimitive(PRIMITIVE_ARRAY_SET_FIRST)) {
                if (SymExpr* wide = toSymExpr(call->get(3))) {
                  Type* type = wide->var->type;
                  VarSymbol* tmp = new VarSymbol("tmp", wideClassMap.get(type));
                  call->getStmtExpr()->insertBefore(new DefExpr(tmp));
                  se->replace(new SymExpr(tmp));
                  call->getStmtExpr()->insertBefore(new CallExpr(PRIMITIVE_MOVE, tmp, se));
                }
              }
            }
          }
        }
      }
    }
  }


  //
  // Turn calls to extern functions involving wide classes into moves
  // of the wide class into a non-wide type and then use that in the call.
  // After the call, copy the value back into the wide class.
  //
  forv_Vec(BaseAST, ast, gAsts) {
    if (CallExpr* call = toCallExpr(ast)) {
      if (call->isResolved() && call->isResolved()->hasPragma(PRAG_EXTERN)) {
        for_alist(arg, call->argList) {
          SymExpr* sym = toSymExpr(arg);
          INT_ASSERT(sym);
          if (sym->typeInfo()->symbol->hasPragma(PRAG_WIDE_CLASS) ||
              sym->typeInfo()->symbol->hasPragma(PRAG_WIDE)) {
            VarSymbol* var = new VarSymbol("_tmp", sym->typeInfo()->getField("addr")->type);
            call->getStmtExpr()->insertBefore(new DefExpr(var));
            if (var->type->symbol->hasPragma(PRAG_REF))
              call->getStmtExpr()->insertBefore(new CallExpr(PRIMITIVE_MOVE, var, sym->copy()));
            else
              call->getStmtExpr()->insertBefore(new CallExpr(PRIMITIVE_MOVE, var, new CallExpr(PRIMITIVE_GET_REF, sym->copy())));
            call->getStmtExpr()->insertAfter(new CallExpr(PRIMITIVE_MOVE, sym->copy(), var));
            sym->replace(new SymExpr(var));
          }
        }
      }
    }
  }



  //
  // insert wide class temps for nil
  //
  forv_Vec(BaseAST, ast, gAsts) {
    if (SymExpr* se = toSymExpr(ast)) {
      if (se->var == gNil || se->var == gNilRef) {
        if (CallExpr* call = toCallExpr(se->parentExpr)) {
          if (call->isResolved()) {
            if (Type* type = actual_to_formal(se)->typeInfo()) {
              if (type->symbol->hasPragma(PRAG_WIDE_CLASS)) {
                VarSymbol* tmp = new VarSymbol("_tmp", type);
                call->getStmtExpr()->insertBefore(new DefExpr(tmp));
                se->replace(new SymExpr(tmp));
                call->getStmtExpr()->insertBefore(new CallExpr(PRIMITIVE_MOVE, tmp, se));
              }
            }
          } else if (call->isPrimitive(PRIMITIVE_MOVE)) {
            if (Type* wtype = call->get(1)->typeInfo()) {
              if (wtype->symbol->hasPragma(PRAG_WIDE)) {
                if (Type* wctype = wtype->getField("addr")->type->getField("_val")->type) {
                  if (wctype->symbol->hasPragma(PRAG_WIDE_CLASS)) {
                    VarSymbol* tmp = new VarSymbol("_tmp", wctype);
                    call->getStmtExpr()->insertBefore(new DefExpr(tmp));
                    se->replace(new SymExpr(tmp));
                    call->getStmtExpr()->insertBefore(new CallExpr(PRIMITIVE_MOVE, tmp, se));
                  }
                }
              }
            }
          } else if (call->isPrimitive(PRIMITIVE_SET_MEMBER)) {
            if (Type* wctype = call->get(2)->typeInfo()) {
              if (wctype->symbol->hasPragma(PRAG_WIDE_CLASS) ||
                  wctype->symbol->hasPragma(PRAG_WIDE)) {
                VarSymbol* tmp = new VarSymbol("_tmp", wctype);
                call->getStmtExpr()->insertBefore(new DefExpr(tmp));
                se->replace(new SymExpr(tmp));
                call->getStmtExpr()->insertBefore(new CallExpr(PRIMITIVE_MOVE, tmp, se));
              }
            }
          }
        }
      }
    }
  }



  //
  // insert cast temps if lhs type does not match cast type
  //   allows separation of the remote put with the wide cast
  //
  forv_Vec(BaseAST, ast, gAsts) {
    if (CallExpr* call = toCallExpr(ast)) {
      if (call->isPrimitive(PRIMITIVE_CAST)) {
        if (CallExpr* move = toCallExpr(call->parentExpr)) {
          if (move->isPrimitive(PRIMITIVE_MOVE)) {
            if (move->get(1)->typeInfo() != call->typeInfo()) {
              VarSymbol* tmp = new VarSymbol("_tmp", call->typeInfo());
              move->insertBefore(new DefExpr(tmp));
              call->replace(new SymExpr(tmp));
              move->insertBefore(new CallExpr(PRIMITIVE_MOVE, tmp, call));
            }
          }
        }
      }
    }
  }

  //
  // initialize global variables across locales
  //
  Map<Symbol*,Vec<SymExpr*>*> defMap;
  Map<Symbol*,Vec<SymExpr*>*> useMap;
  buildDefUseMaps(defMap, useMap); // for "ack!!" below

  Vec<Symbol*> heapVars;
  forv_Vec(BaseAST, ast, gAsts) {
    if (CallExpr* call = toCallExpr(ast)) {

      //
      // if this is a call to _heapAllocGlobal or
      // _heapAllocConstGlobal (or a wrapper thereof) but not from
      // within a wrapper, then this is a global variable
      //
      if ((call->isNamed("_heapAllocGlobal") ||
           call->isNamed("_heapAllocConstGlobal")) &&
          !toFnSymbol(call->parentSymbol)->isWrapper) {

        CallExpr* move = toCallExpr(call->parentExpr);
        INT_ASSERT(move);
        SymExpr* lhs = toSymExpr(move->get(1));
        INT_ASSERT(lhs);

        //
        // ack!! check for tmp before global
        // this can happen due to coercion wrappers
        // this is a worrisome fix
        //
        if (lhs->var->isCompilerTemp) {
          if (CallExpr* move2 = toCallExpr(move->next)) {
            Vec<SymExpr*>* defs = defMap.get(lhs->var);
            Vec<SymExpr*>* uses = useMap.get(lhs->var);
            if (defs && defs->n == 1 &&
                uses && uses->n == 1 &&
                uses->v[0] == move2->get(2)) {
              move2->get(2)->replace(call->remove());
              move->remove();
              move = move2;
              lhs->var->defPoint->remove();
              lhs = toSymExpr(move->get(1));
            }
          }
        }

        if (lhs->var->type->symbol->hasPragma(PRAG_WIDE_CLASS)) {

          //
          // handle global variables on the heap
          //
          move->replace(new CallExpr(PRIMITIVE_SET_MEMBER, lhs->remove(), lhs->var->type->getField("addr")->type->getField("_val"), call->get(1)->remove()));
          heapVars.add(lhs->var);

        } else {

          //
          // handle constant global variables not on the heap
          //
          call->replace(call->get(1)->remove());
          move->insertAfter(new CallExpr(PRIMITIVE_PRIVATE_BROADCAST, lhs->var));
        }
      }
    }
  }

  freeDefUseMaps(defMap, useMap);

  //
  // dereference wide string actual argument to primitive
  //
  forv_Vec(BaseAST, ast, gAsts) {
    if (CallExpr* call = toCallExpr(ast)) {
      if (call->primitive) {
        if (call->primitive->tag == PRIMITIVE_UNKNOWN ||
            call->isPrimitive(PRIMITIVE_CAST)) {
          for_actuals(actual, call) {
            if (actual->typeInfo()->symbol->hasPragma(PRAG_WIDE_CLASS)) {
              if (actual->typeInfo()->getField("addr")->typeInfo() == dtString) {
                VarSymbol* tmp = new VarSymbol("tmp", actual->typeInfo()->getField("addr")->typeInfo());
                call->getStmtExpr()->insertBefore(new DefExpr(tmp));
                call->getStmtExpr()->insertBefore(new CallExpr(PRIMITIVE_MOVE, tmp, new CallExpr(PRIMITIVE_GET_REF, actual->copy())));
                actual->replace(new SymExpr(tmp));
              }
            }
          }
        }
      }
    }
  }

  handleLocalBlocks();

  CallExpr* localeID = new CallExpr(PRIMITIVE_LOCALE_ID);
  VarSymbol* tmp = new VarSymbol("_tmp", localeID->typeInfo());
  VarSymbol* tmpBool = new VarSymbol("_tmp", dtBool);
  tmp->isCompilerTemp = true;
  tmpBool->isCompilerTemp = true;

  heapAllocateGlobals->insertAtTail(new DefExpr(tmp));
  heapAllocateGlobals->insertAtTail(new DefExpr(tmpBool));
  heapAllocateGlobals->insertAtTail(new CallExpr(PRIMITIVE_MOVE, tmp, localeID));
  heapAllocateGlobals->insertAtTail(new CallExpr(PRIMITIVE_MOVE, tmpBool, new CallExpr(PRIMITIVE_EQUAL, tmp, new_IntSymbol(0))));
  BlockStmt* block = new BlockStmt();
  forv_Vec(Symbol, sym, heapVars) {
    block->insertAtTail(new CallExpr(PRIMITIVE_MOVE, sym, new CallExpr(PRIMITIVE_CHPL_ALLOC, sym->type->getField("addr")->type->symbol, new_StringSymbol("global var heap allocation"))));
    block->insertAtTail(new CallExpr(PRIMITIVE_SETCID, sym));
  }
  heapAllocateGlobals->insertAtTail(new CondStmt(new SymExpr(tmpBool), block));
  int i = 0;
  forv_Vec(Symbol, sym, heapVars) {
    heapAllocateGlobals->insertAtTail(new CallExpr(PRIMITIVE_HEAP_REGISTER_GLOBAL_VAR, new_IntSymbol(i++), sym));
  }
  heapAllocateGlobals->insertAtTail(new CallExpr(PRIMITIVE_HEAP_BROADCAST_GLOBAL_VARS, new_IntSymbol(i)));
  heapAllocateGlobals->insertAtTail(new CallExpr(PRIMITIVE_RETURN, gVoid));
  numGlobalsOnHeap = i;
}
