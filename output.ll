; ModuleID = 'kawa_main'
source_filename = "kawa_main"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-darwin25.1.0"

%User = type { i32, i8 }

@str.2 = private unnamed_addr constant [7 x i8] c"%d %d\0A\00", align 1
@str.3 = private unnamed_addr constant [29 x i8] c"Char: %c, Bool: %d, Mil: %d\0A\00", align 1
@str.4 = private unnamed_addr constant [19 x i8] c"Orbit: x=%d, y=%d\0A\00", align 1
@str.5 = private unnamed_addr constant [27 x i8] c"Orbit Updated: x=%d, y=%d\0A\00", align 1
@str.6 = private unnamed_addr constant [11 x i8] c"Batch: %d\0A\00", align 1
@str.7 = private unnamed_addr constant [21 x i8] c"Async Done. Sum: %d\0A\00", align 1
@str.9 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@str.11 = private unnamed_addr constant [16 x i8] c"Main working...\00", align 1
@str.12 = private unnamed_addr constant [5 x i8] c"End.\00", align 1

; Function Attrs: mustprogress nofree nounwind willreturn allockind("alloc,uninitialized") allocsize(0) memory(inaccessiblemem: readwrite)
declare noalias noundef ptr @malloc(i64 noundef) local_unnamed_addr #0

; Function Attrs: mustprogress nounwind willreturn allockind("realloc") allocsize(1) memory(argmem: readwrite, inaccessiblemem: readwrite)
declare noalias noundef ptr @realloc(ptr allocptr captures(none), i64 noundef) local_unnamed_addr #1

; Function Attrs: nofree nounwind
define i32 @User__add(ptr readnone captures(none) %0, %User %1, %User %2) local_unnamed_addr #2 {
entry:
  %.fca.0.extract = extractvalue %User %1, 0
  %.fca.1.extract = extractvalue %User %1, 1
  %.fca.0.extract14 = extractvalue %User %2, 0
  %.fca.1.extract15 = extractvalue %User %2, 1
  %add = add nsw i32 %.fca.0.extract14, %.fca.0.extract
  %add7 = add nsw i8 %.fca.1.extract15, %.fca.1.extract
  %vararg_prom = zext i8 %add7 to i32
  %3 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @str.2, i32 %add, i32 %vararg_prom)
  ret i32 %add
}

; Function Attrs: nofree nounwind
declare noundef i32 @printf(ptr noundef readonly captures(none), ...) local_unnamed_addr #2

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define noundef i32 @User__add.1(ptr readnone captures(none) %0, ptr readnone captures(none) %1) local_unnamed_addr #3 {
entry:
  ret i32 0
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define range(i32 -2147483647, -2147483648) i32 @test(i32 %0) local_unnamed_addr #3 {
entry:
  %add = add nsw i32 %0, 1
  ret i32 %add
}

; Function Attrs: nounwind
define noundef i32 @main() local_unnamed_addr #4 {
sip_cont:
  %0 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @str.2, i32 0, i32 90)
  %1 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @str.2, i32 92, i32 58)
  %2 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @str.3, i32 75, i32 1, i32 21)
  %3 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @str.4, i32 10, i32 15)
  %4 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @str.5, i32 50, i32 15)
  %malloc = tail call dereferenceable_or_null(8) ptr @malloc(i64 8)
  store <2 x i32> <i32 1, i32 2>, ptr %malloc, align 4, !tbaa !0
  %new_mem = tail call dereferenceable_or_null(16) ptr @realloc(ptr nonnull %malloc, i64 16)
  %item = load i32, ptr %new_mem, align 4, !tbaa !0
  %5 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @str.6, i32 %item)
  %item_ptr.1 = getelementptr i8, ptr %new_mem, i64 4
  %item.1 = load i32, ptr %item_ptr.1, align 4, !tbaa !0
  %6 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @str.6, i32 %item.1)
  %7 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @str.6, i32 3)
  %coro_mem.i = tail call dereferenceable_or_null(24) ptr @malloc(i64 24)
  store ptr @kawa_task_0.resume, ptr %coro_mem.i, align 8
  %destroy.addr.i = getelementptr inbounds nuw i8, ptr %coro_mem.i, i64 8
  store ptr @kawa_task_0.destroy, ptr %destroy.addr.i, align 8
  %index.addr6.i = getelementptr inbounds nuw i8, ptr %coro_mem.i, i64 20
  store i1 false, ptr %index.addr6.i, align 1
  %puts = tail call i32 @puts(ptr nonnull dereferenceable(1) @str.11)
  %promise_storage.reload.addr.i = getelementptr inbounds nuw i8, ptr %coro_mem.i, i64 16
  %8 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @str.7, i32 1000000)
  store volatile i32 1000000, ptr %promise_storage.reload.addr.i, align 4
  %sip_val = load volatile i32, ptr %promise_storage.reload.addr.i, align 4, !tbaa !0
  %9 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @str.9, i32 %sip_val)
  %puts37 = tail call i32 @puts(ptr nonnull dereferenceable(1) @str.12)
  ret i32 0
}

; Function Attrs: mustprogress nofree nounwind willreturn memory(write, argmem: none, inaccessiblemem: readwrite)
define noalias noundef ptr @kawa_task_0() local_unnamed_addr #5 {
entry:
  %coro_mem = tail call dereferenceable_or_null(24) ptr @malloc(i64 24)
  store ptr @kawa_task_0.resume, ptr %coro_mem, align 8
  %destroy.addr = getelementptr inbounds nuw i8, ptr %coro_mem, i64 8
  store ptr @kawa_task_0.destroy, ptr %destroy.addr, align 8
  %index.addr6 = getelementptr inbounds nuw i8, ptr %coro_mem, i64 20
  store i1 false, ptr %index.addr6, align 1
  ret ptr %coro_mem
}

; Function Attrs: nofree nounwind
define internal fastcc void @kawa_task_0.resume(ptr noundef nonnull align 8 dereferenceable(24) %hdl) #2 {
entry.resume:
  %promise_storage.reload.addr = getelementptr inbounds nuw i8, ptr %hdl, i64 16
  %0 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @str.7, i32 1000000)
  store volatile i32 1000000, ptr %promise_storage.reload.addr, align 8
  ret void
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define internal fastcc void @kawa_task_0.destroy(ptr nonnull readnone align 8 captures(none) %hdl) #3 {
entry.destroy:
  ret void
}

; Function Attrs: nofree nounwind
declare noundef i32 @puts(ptr noundef readonly captures(none)) local_unnamed_addr #2

attributes #0 = { mustprogress nofree nounwind willreturn allockind("alloc,uninitialized") allocsize(0) memory(inaccessiblemem: readwrite) "alloc-family"="malloc" }
attributes #1 = { mustprogress nounwind willreturn allockind("realloc") allocsize(1) memory(argmem: readwrite, inaccessiblemem: readwrite) "alloc-family"="malloc" }
attributes #2 = { nofree nounwind }
attributes #3 = { mustprogress nofree norecurse nosync nounwind willreturn memory(none) }
attributes #4 = { nounwind }
attributes #5 = { mustprogress nofree nounwind willreturn memory(write, argmem: none, inaccessiblemem: readwrite) }

!0 = !{!1, !1, i64 0}
!1 = !{!"tbaa_int", !2}
!2 = !{!"tbaa_scalar", !3}
!3 = !{!"Kawa TBAA"}
