; ModuleID = 'kawa_main'
source_filename = "kawa_main"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-darwin25.5.0"

@str = private unnamed_addr constant [11 x i8] c"Batch: %d\0A\00", align 1

; Function Attrs: mustprogress nofree nounwind willreturn allockind("alloc,uninitialized") allocsize(0) memory(inaccessiblemem: readwrite)
declare noalias noundef ptr @malloc(i64 noundef) local_unnamed_addr #0

; Function Attrs: mustprogress nounwind willreturn allockind("realloc") allocsize(1) memory(argmem: readwrite, inaccessiblemem: readwrite)
declare noalias noundef ptr @realloc(ptr allocptr captures(none), i64 noundef) local_unnamed_addr #1

; Function Attrs: nounwind
define noundef i32 @main() local_unnamed_addr #2 {
entry:
  %malloc = tail call dereferenceable_or_null(8) ptr @malloc(i64 8)
  store <2 x i32> <i32 1, i32 2>, ptr %malloc, align 4, !tbaa !0
  %new_mem = tail call dereferenceable_or_null(16) ptr @realloc(ptr nonnull %malloc, i64 16)
  %item = load i32, ptr %new_mem, align 4, !tbaa !0
  %0 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @str, i32 %item)
  %item_ptr.1 = getelementptr i8, ptr %new_mem, i64 4
  %item.1 = load i32, ptr %item_ptr.1, align 4, !tbaa !0
  %1 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @str, i32 %item.1)
  %2 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @str, i32 3)
  ret i32 0
}

; Function Attrs: nofree nounwind
declare noundef i32 @printf(ptr noundef readonly captures(none), ...) local_unnamed_addr #3

attributes #0 = { mustprogress nofree nounwind willreturn allockind("alloc,uninitialized") allocsize(0) memory(inaccessiblemem: readwrite) "alloc-family"="malloc" }
attributes #1 = { mustprogress nounwind willreturn allockind("realloc") allocsize(1) memory(argmem: readwrite, inaccessiblemem: readwrite) "alloc-family"="malloc" }
attributes #2 = { nounwind }
attributes #3 = { nofree nounwind }

!0 = !{!1, !1, i64 0}
!1 = !{!"tbaa_int", !2}
!2 = !{!"tbaa_scalar", !3}
!3 = !{!4}
!4 = !{!3}
