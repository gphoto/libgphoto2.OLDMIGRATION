Working with git
================


Simple intro to using git with the gphoto source code.


Get source code and keep it current
-----------------------------------

*   Initial cloning of the source repo:

        $ git clone https://github.com/gphoto/libgphoto2
        $ cd libgphoto2

*   Merge upstream changes into the local master branch:

        $ cd libgphoto2
        $ git pull

    This makes the most sense if you have not changed anything locally.

*   Fetch upstream changes, but do not merge them:

        $ cd libgphoto2
        $ git fetch -v

*   Get a graphical overview of what you are doing

        $ cd libgphoto2
        $ gitk --all


Contribute code (as gphoto developer)
-------------------------------------

To be written.


Contribute code (as anyone else)
-------------------------------------

To be written.


Tips and tricks
---------------

* How to discard all changes to all `*.po` files (e.g. useful after
  `make dist`):

        git checkout -- po/*.po libgphoto2_port/po/*.po

* Committing changes to the `*.m4` files in `m4/` and
  `libgphoto2_port/m4/` is non-trivial. The description for doing that
  still needs to be written.

  However, those files change rarely, and only few people ever change
  them, so it is only those few people who need that specific
  knowledge. Everyone else can just clone and pull their repos and
  things work.
